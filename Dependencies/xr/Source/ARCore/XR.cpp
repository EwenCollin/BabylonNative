#include <XR.h>
#include <XRHelpers.h>

#include <assert.h>
#include <optional>
#include <sstream>
#include <chrono>
#include <arcana/threading/task.h>
#include <arcana/threading/dispatcher.h>
#include <thread>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <AndroidExtensions/Globals.h>
#include <AndroidExtensions/JavaWrappers.h>
#include <AndroidExtensions/OpenGLHelpers.h>
#include <AndroidExtensions/Permissions.h>
#include <android/native_window.h>
#include <android/log.h>
#include <arcore_c_api.h>

#include <gsl/gsl>

#define GLM_FORCE_RADIANS 1
#define GLM_ENABLE_EXPERIMENTAL
#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>
#include <gtx/quaternion.hpp>
#include <arcana/threading/task_schedulers.h>

#include "Include/IXrContextARCore.h"

#include <inttypes.h>
#include <unordered_map>

#include <android/log.h>
#define  LOG_TAG    "NativeLOG"

#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace android;
using namespace android::global;
using namespace android::OpenGLHelpers;

namespace xr
{
    struct XrContextARCore : public IXrContextARCore {
        bool Initialized{false};
        ArSession* Session{nullptr};
        ArFrame* Frame{nullptr};
        ArEarth* Earth{nullptr};
        std::unordered_map<std::string, std::shared_ptr<ArAnchor*>> EarthAnchors{};
        std::unordered_map<std::string, std::shared_ptr<ArResolveAnchorOnTerrainFuture*>> Futures{};
        std::unordered_map<std::string, std::shared_ptr<ArHostCloudAnchorFuture*>> CloudAnchorHostingFutures{};
        std::unordered_map<std::string, std::shared_ptr<ArResolveCloudAnchorFuture*>> CloudAnchorResolvingFutures{};

        bool IsInitialized() const override
        {
            return Initialized;
        }

        ArSession* XrSession() const override
        {
            return Session;
        }

        ArFrame* XrFrame() const override
        {
            return Frame;
        }

        ArEarth* XrEarth() const override
        {
            return Earth;
        }
        std::unordered_map<std::string, std::shared_ptr<ArAnchor*>> XrEarthAnchors() const override
        {
            return EarthAnchors;
        }
        std::unordered_map<std::string, std::shared_ptr<ArResolveAnchorOnTerrainFuture*>> XrFutures() const override
        {
            return Futures;
        }
        std::unordered_map<std::string, std::shared_ptr<ArHostCloudAnchorFuture*>> XrCloudAnchorHostingFutures() const override
        {
            return CloudAnchorHostingFutures;
        }
        std::unordered_map<std::string, std::shared_ptr<ArResolveCloudAnchorFuture*>> XrCloudAnchorResolvingFutures() const override
        {
            return CloudAnchorResolvingFutures;
        }

        virtual ~XrContextARCore() = default;
    };

    struct System::Impl
    {
        std::shared_ptr<XrContextARCore> XrContext{std::make_shared<XrContextARCore>()};

        Impl(const std::string& /*applicationName*/)
        {
        }

        bool IsInitialized() const
        {
            return true;
        }

        bool TryInitialize() {
            return true;
        }
    };

    namespace
    {
        constexpr GLfloat VERTEX_POSITIONS[]{ -1.0f, -1.0f, +1.0f, -1.0f, -1.0f, +1.0f, +1.0f, +1.0f };
        constexpr size_t VERTEX_COUNT{ std::size(VERTEX_POSITIONS) / 2 };

        constexpr char CAMERA_VERT_SHADER[]{ R"(#version 300 es
            precision highp float;
            uniform vec2 vertexPositions[4];
            uniform vec2 cameraFrameUVs[4];
            out vec2 cameraFrameUV;
            void main() {
                gl_Position = vec4(vertexPositions[gl_VertexID], 0.0, 1.0);
                cameraFrameUV = cameraFrameUVs[gl_VertexID];
            }
        )"};

        constexpr char BABYLON_VERT_SHADER[]{ R"(#version 300 es
            precision highp float;
            uniform vec2 vertexPositions[4];
            uniform vec2 cameraFrameUVs[4];
            out vec2 cameraFrameUV;
            out vec2 babylonUV;
            void main() {
                gl_Position = vec4(vertexPositions[gl_VertexID], 0.0, 1.0);
                babylonUV = vec2(gl_Position.x + 1.0, gl_Position.y + 1.0) * 0.5;
                cameraFrameUV = cameraFrameUVs[gl_VertexID];
            }
        )"};

        constexpr char CAMERA_FRAG_SHADER[]{ R"(#version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision mediump float;
            in vec2 cameraFrameUV;
            uniform samplerExternalOES cameraTexture;
            // Location 0 is GL_COLOR_ATTACHMENT0, which in turn is the babylonTexture
            layout(location = 0) out vec4 oFragColor;
            void main() {
                vec4 camColor = texture(cameraTexture, cameraFrameUV);
                //camColor.z = (floor(camColor.z * 255.0) * 256.0) / 65535.0;
                //camColor *= step(cameraFrameUV.y * 2.0, 1.0);
                oFragColor = vec4(0.0, 0.0, 0.0, 0.0);//camColor;
            }
        )"};

        constexpr char BABYLON_FRAG_SHADER[]{ R"(#version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision highp float;
            in vec2 babylonUV;
            uniform sampler2D babylonTexture;
            uniform sampler2D depthTexture;
            in vec2 cameraFrameUV;
            uniform samplerExternalOES cameraTexture;
            uniform sampler2D babylonTextureCopy;
            out vec4 oFragColor;
            const float kMidDepthMeters = 8.0;
            const float kMaxDepthMeters = 30.0;
            
            float DepthGetMillimeters(in sampler2D depth_texture, in vec2 depth_uv) {
              // Depth is packed into the red and green components of its texture.
              // The texture is a normalized format, storing millimeters.
              vec3 packedDepthAndVisibility = texture(depth_texture, depth_uv).xyz;
              return dot(packedDepthAndVisibility.xy, vec2(255.0, 256.0 * 255.0));
            }
            
            // Returns linear interpolation position of value between min and max bounds.
            // E.g. InverseLerp(1100, 1000, 2000) returns 0.1.
            float InverseLerp(float value, float min_bound, float max_bound) {
              return clamp((value - min_bound) / (max_bound - min_bound), 0.0, 1.0);
            }
            
            //float unpackDepth(float packedValue) {
            //    return mod(packedValue * 16383.0, 128.0) / 127.0;
            //}

            //float unpackAlpha(float packedValue) {
            //    return floor(mod(floor(packedValue * 16383.0 / 128.0), 128.0)) / 127.0;
            //}

            float unpackDepth(float packedValue) {
                return (packedValue - (floor(packedValue * 10.0) * 0.1)) * 10.0;
            }

            float unpackAlpha(float packedValue) {
                return floor(packedValue * 10.0) * 0.1;
            }

            float DepthGetVisibility(in sampler2D depth_texture, in vec2 depth_uv,
                                     in float asset_depth_mm) {
              float depth_mm = DepthGetMillimeters(depth_texture, depth_uv);
            
              // Instead of a hard Z-buffer test, allow the asset to fade into the
              // background along a 2 * kDepthTolerancePerMm * asset_depth_mm
              // range centered on the background depth.
              const float kDepthTolerancePerMm = 0.015f;
              float visibility_occlusion = clamp(0.5 * (depth_mm - asset_depth_mm) /
                (kDepthTolerancePerMm * asset_depth_mm) + 0.5, 0.0, 1.0);
            
             // Use visibility_depth_near to set the minimum depth value. If using
             // this value for occlusion, avoid setting it too close to zero. A depth value
             // of zero signifies that there is no depth data to be found.
              float visibility_depth_near = 1.0 - InverseLerp(
                  depth_mm, /*min_depth_mm=*/150.0, /*max_depth_mm=*/200.0);
            
              // Use visibility_depth_far to set the maximum depth value. If the depth
              // value is too high (outside the range specified by visibility_depth_far),
              // the virtual object may get inaccurately occluded at further distances
              // due to too much noise.
              float visibility_depth_far = InverseLerp(
                  depth_mm, /*min_depth_mm=*/7500.0, /*max_depth_mm=*/8000.0);
            
              const float kOcclusionAlpha = 0.0f;
              float visibility =
                  max(max(visibility_occlusion, kOcclusionAlpha),
                      max(visibility_depth_near, visibility_depth_far));
            
              return visibility;
            }

            vec3 hsv2rgb(vec3 c)
            {
                vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
                vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
                return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
            }

            
            void main() {
                vec4 camColor = texture(cameraTexture, cameraFrameUV);
                vec4 baseGameColor = texture(babylonTextureCopy, babylonUV);
                vec4 gameColor = baseGameColor;
                vec4 controlColor = texture(babylonTextureCopy, vec2(0.0, 0.0));

                float is_control_uv = 0.0;//step(-0.001, babylonUV.x + babylonUV.y) * (1.0 - step(0.001, babylonUV.x + babylonUV.y));

                vec2 dUV = cameraFrameUV;//vec2(1.0 - babylonUV.y, 1.0 - babylonUV.x);
                float visibility = DepthGetVisibility(depthTexture, dUV, gameColor.z * 16.0 * 1000.0);//unpackDepth(gameColor.z) * 16.0 * 1000.0);//
                vec3 rgb = hsv2rgb(vec3(gameColor.x, gameColor.y, unpackDepth(gameColor.y)));
                //gameColor.z = 0.0;
                //gameColor.z = unpackAlpha(gameColor.z);
                gameColor.a = step(0.01, gameColor.r + gameColor.g + gameColor.b) * visibility;// step(0.001, visibility);
                gameColor.r = rgb.r;
                gameColor.g = rgb.g;
                gameColor.b = rgb.b;
                vec4 baseColor = mix(camColor, gameColor, gameColor.a);

                //baseColor = mix(baseColor, baseGameColor, step(1.999, controlColor.r + controlColor.g));
                //baseColor = mix(baseColor, vec4(1.0, 1.0, 0.0, 1.0), is_control_uv); 
                oFragColor = baseColor;
                













                //vec4 camColor = texture(babylonTexture, vec2(babylonUV.x * 0.5, babylonUV.y));
                //vec4 gameColor = texture(babylonTexture, babylonUV);//texture(babylonTexture, vec2((babylonUV.x * 0.5) + 0.5, babylonUV.y));

                //vec2 dUV = vec2(1.0 - babylonUV.y, 1.0 - babylonUV.x);
                //float visibility = DepthGetVisibility(depthTexture, dUV, unpackDepth(gameColor.z) * 64.0 * 1000.0);
                //gameColor.x *= visibility;
                //gameColor.y *= visibility;
                //gameColor.z *= visibility;
                //gameColor.a = visibility;
                //vec4 baseColor = gameColor;
                //vec4 baseColor = mix(camColor, gameColor, gameColor.a);//texture(babylonTexture, babylonUV);
                //baseColor.w = 1.0;
                //baseColor.w = 0.0;
                //vec2 dUV = vec2(1.0 - babylonUV.y, 1.0 - babylonUV.x);
                //float visibility = DepthGetVisibility(depthTexture, dUV, unpackDepth(baseColor.z) * 64.0 * 1000.0);
                //baseColor.w = baseColor.w * visibility;
                //baseColor.x = DepthGetMillimeters(depthTexture, dUV) * 0.0005;
                //baseColor.y = 0.0;
                //oFragColor = baseColor;  //Depth texture visualization only (testing)
            }
        )"};

        bool CheckARCoreInstallStatus(bool requestInstall)
        {
            ArInstallStatus install_status;
            ArStatus installStatus{ ArCoreApk_requestInstall(GetEnvForCurrentThread(), GetCurrentActivity(), requestInstall, &install_status) };
            return installStatus == AR_SUCCESS && install_status == AR_INSTALL_STATUS_INSTALLED;
        }

        arcana::task<void, std::exception_ptr> CheckAndInstallARCoreAsync()
        {
            auto task{ arcana::task_from_result<std::exception_ptr>() };

            // Check if ARCore is already installed.
            if (!CheckARCoreInstallStatus(false))
            {
                arcana::task_completion_source<void, std::exception_ptr> installTcs{};

                // Add a resume callback, which will check if ARCore has been successfully installed upon app resume.
                auto resumeTicket{AddResumeCallback([installTcs]() mutable {
                    if (!CheckARCoreInstallStatus(false))
                    {
                        // ARCore not installed, throw an error.
                        std::ostringstream message;
                        message << "ARCore not installed.";
                        installTcs.complete(arcana::make_unexpected(make_exception_ptr(std::runtime_error{message.str()})));
                    }
                    else
                    {
                        // ARCore installed successfully, complete the promise.
                        installTcs.complete();
                    }
                })};

                // Kick off the install request, and set the task for our caller to wait on.
                CheckARCoreInstallStatus(true);
                task = installTcs.as_task().then(arcana::inline_scheduler, arcana::cancellation::none(), [resumeTicket = std::move(resumeTicket)](){
                    return;
                });
            }

            return task;
        }

        // Converts an image bitmap to grayscale.
        // Supported image formats: RBG(A)8, RGB(A)16, 16-bit grayscale.
        void ConvertBitmapToGrayscale(
                const uint8_t* imagePixelBuffer,
                const int32_t width,
                const int32_t height,
                const int32_t stride,
                uint8_t* grayscaleBuffer)
        {
            const std::size_t pixelStride {static_cast<std::size_t>(stride / width)};
            for (int h{0}; h < height; ++h)
            {
                for (int w{0}; w < width; ++w)
                {
                    gsl::span<const uint8_t> pixel{imagePixelBuffer + (w * pixelStride + h * stride), pixelStride};

                    if (pixelStride == 2)
                    {
                        // 16-bit grayscale
                        gsl::span<const uint16_t> pixel16{reinterpret_cast<const uint16_t *>(pixel.data()), 1};
                        grayscaleBuffer[w + h * width] = pixel16[0] / 257;
                    }
                    else if (pixelStride == 4 || pixelStride == 3)
                    {
                        // RGB8/RGBA8
                        const uint8_t r{pixel[0]};
                        const uint8_t g{pixel[1]};
                        const uint8_t b{pixel[2]};
                        grayscaleBuffer[w + h * width] = static_cast<uint8_t>(
                            0.213f * r +
                            0.715 * g +
                            0.072 * b);
                    }
                    else if (pixelStride == 6 || pixelStride == 8)
                    {
                        // RGB16/RGBA16
                        gsl::span<const uint16_t> pixel16{reinterpret_cast<const uint16_t *>(pixel.data()), pixelStride / 2};
                        const uint8_t r{static_cast<uint8_t>(pixel16[0] / 257)};
                        const uint8_t g{static_cast<uint8_t>(pixel16[1] / 257)};
                        const uint16_t b{static_cast<uint8_t>(pixel16[2] / 257)};
                        grayscaleBuffer[w + h * width] = static_cast<uint8_t>(
                            0.213f * r +
                            0.715 * g +
                            0.072 * b);
                    }
                }
            }
        }
    }

    struct System::Session::Impl
    {
        using EGLSurfacePtr = std::unique_ptr<std::remove_pointer_t<EGLSurface>, std::function<void(EGLSurface)>>;

        const System::Impl& SystemImpl;
        std::vector<Frame::View> ActiveFrameViews{ {} };
        std::vector<Frame::InputSource> InputSources{};
        std::vector<Frame::Plane> Planes{};
        std::vector<Frame::Mesh> Meshes{};
        std::vector<FeaturePoint> FeaturePointCloud{};
        std::optional<Space> EyeTrackerSpace{};
        float DepthNearZ{ DEFAULT_DEPTH_NEAR_Z };
        float DepthFarZ{ DEFAULT_DEPTH_FAR_Z };
        bool PlaneDetectionEnabled{ false };
        bool FeaturePointCloudEnabled{ false };

        Impl(System::Impl& systemImpl, void* graphicsContext, std::function<void*()> windowProvider)
            : SystemImpl{ systemImpl }
            , xrContext{systemImpl.XrContext}
            , windowProvider{ [windowProvider{ std::move(windowProvider) }] { return reinterpret_cast<ANativeWindow*>(windowProvider()); } }
            , context{reinterpret_cast<EGLContext>(graphicsContext) }
            , pauseTicket{AddPauseCallback([this]() { this->PauseSession(); }) }
            , resumeTicket{AddResumeCallback([this]() { this->ResumeSession(); }) }
        {
        }

        ~Impl()
        {
            if (xrContext->Initialized)
            {
                Planes.clear();
                CleanupAnchor(nullptr);
                CleanupFrameTrackables();
                CleanupImageTrackingTrackables();
                ArPose_destroy(cameraPose);
                ArPose_destroy(tempPose);
                ArHitResult_destroy(hitResult);
                ArHitResultList_destroy(hitResultList);
                ArTrackableList_destroy(trackableList);
                if (augmentedImageDatabase != nullptr)
                {
                    ArAugmentedImageDatabase_destroy(augmentedImageDatabase);
                }

                ArFrame_destroy(xrContext->Frame);
                xrContext->Frame = nullptr;
                ArSession_destroy(xrContext->Session);
                xrContext->Session = nullptr;
                xrContext->Initialized = false;
                ArTrackable_release(reinterpret_cast<ArTrackable*>(xrContext->Earth));

                glDeleteTextures(1, &cameraTextureId);
                glDeleteTextures(1, &depthTextureId);
                glDeleteTextures(1, &babylonTextureCopyId);
                glDeleteProgram(cameraShaderProgramId);
                glDeleteProgram(babylonShaderProgramId);
                glDeleteFramebuffers(1, &cameraFrameBufferId);
                glDeleteFramebuffers(1, &babylonTextureCopyFrameBufferId);

                DestroyDisplayResources();
            }
        }

        void Initialize()
        {
            {
                // Get the config from the current surface.
                // This assumes a surface has already been created for the context that was passed in.
                // If not, it will fail, but in the context of Babylon Native, this should always work.
                // If the same exact config is not used, the eglMakeCurrent will fail, and XR rendering
                // will not work. Getting the config from the current surface is a more reliable way
                // of ensuring this than trying to manually specify the same attributes to search for a
                // config.

                display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
                if (display == EGL_NO_DISPLAY)
                {
                    throw std::runtime_error{"No default display."};
                }

                EGLSurface currentDrawSurface{ eglGetCurrentSurface(EGL_DRAW) };
                if (currentDrawSurface == EGL_NO_SURFACE)
                {
                    throw std::runtime_error{"No current surface."};
                }

                EGLint configID{};
                if (!eglQuerySurface(display, currentDrawSurface, EGL_CONFIG_ID, &configID))
                {
                    throw std::runtime_error{"Failed to query surface."};
                }

                EGLint numConfigs{};
                if (!eglGetConfigs(display, nullptr, 0, &numConfigs))
                {
                    throw std::runtime_error{"Failed to get configs."};
                }

                std::vector<EGLConfig> configs(numConfigs);
                if (!eglGetConfigs(display, configs.data(), numConfigs, &numConfigs))
                {
                    throw std::runtime_error{"Failed to get configs."};
                }

                auto it = std::find_if(configs.begin(), configs.end(), [display{display}, configID](const auto& config) {
                    EGLint id{};
                    if (!eglGetConfigAttrib(display, config, EGL_CONFIG_ID, &id))
                    {
                        throw std::runtime_error{"Failed to get config attribute."};
                    }
                    return id == configID;
                });

                if (it == configs.end()) {
                    throw std::runtime_error{"Config not found."};
                }

                config = *it;
            }

            // Generate a texture id for the camera texture (ARCore will allocate the texture itself)
            {
                glGenTextures(1, &cameraTextureId);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, cameraTextureId);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
            }

            {
                  glGenTextures(1, &depthTextureId);
                  glBindTexture(GL_TEXTURE_2D, depthTextureId);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            {
                  glGenTextures(1, &babylonTextureCopyId);
                  glBindTexture(GL_TEXTURE_2D, babylonTextureCopyId);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }

            // Create the shader program used for drawing the full screen quad that is the camera frame + Babylon render texture
            cameraShaderProgramId = android::OpenGLHelpers::CreateShaderProgram(CAMERA_VERT_SHADER, CAMERA_FRAG_SHADER);
            babylonShaderProgramId = android::OpenGLHelpers::CreateShaderProgram(BABYLON_VERT_SHADER, BABYLON_FRAG_SHADER);

            // Create the ARCore ArSession
            {
                ArStatus status{ ArSession_create(GetEnvForCurrentThread(), GetAppContext(), &xrContext->Session) };
                if (status != ArStatus::AR_SUCCESS)
                {
                    std::ostringstream message;
                    message << "Failed to create ArSession with status: " << status;
                    throw std::runtime_error{ message.str() };
                }

                // Create the ArConfig
                ArConfig* arConfig{};
                ArConfig_create(xrContext->Session, &arConfig);

                // Set Focus Mode Auto
                ArConfig_setFocusMode(xrContext->Session, arConfig, AR_FOCUS_MODE_AUTO);
                ArConfig_setPlaneFindingMode(xrContext->Session, arConfig, AR_PLANE_FINDING_MODE_HORIZONTAL);
                ArConfig_setGeospatialMode(xrContext->Session, arConfig, AR_GEOSPATIAL_MODE_ENABLED);
                ArConfig_setCloudAnchorMode(xrContext->Session, arConfig,
                                            AR_CLOUD_ANCHOR_MODE_ENABLED);
                // Check whether the user's device supports the Depth API.
                int32_t is_depth_supported = 0;
                ArSession_isDepthModeSupported(xrContext->Session, AR_DEPTH_MODE_AUTOMATIC,
                                               &is_depth_supported);
                if (is_depth_supported) {
                  ArConfig_setDepthMode(xrContext->Session, arConfig, AR_DEPTH_MODE_AUTOMATIC);
                }


                // Configure the ArSession
                ArStatus statusConfig { ArSession_configure(xrContext->Session, arConfig) };

                // Clean up the ArConfig.
                ArConfig_destroy(arConfig);

                if (statusConfig != ArStatus::AR_SUCCESS)
                {
                    // ArSession failed to configure, throw an error
                    std::ostringstream message;
                    message << "Failed to configure ArSession with status: " << status;
                    throw std::runtime_error{ message.str() };
                }
            }

            // Create a frame buffer used for clearing the color texture
            glGenFramebuffers(1, &cameraFrameBufferId);
            glGenFramebuffers(1, &babylonTextureCopyFrameBufferId);

            // Create the ARCore ArFrame (this gets reused each time we query for the latest frame)
            ArFrame_create(xrContext->Session, &xrContext->Frame);

            // Create the ARCore ArPose that tracks camera position
            ArPose_create(xrContext->Session, nullptr, &cameraPose);

            // Create the hit result list, and hit result.
            ArHitResultList_create(xrContext->Session, &hitResultList);
            ArHitResult_create(xrContext->Session, &hitResult);

            // Create the trackable list used to process planes and images.
            ArTrackableList_create(xrContext->Session, &trackableList);

            // Create the reusable ARCore ArPose used for short term operations
            // (i.e. pulling out hit test results, and updating anchors)
            ArPose_create(xrContext->Session, nullptr, &tempPose);

            // Set the texture ID that should be used for the camera frame
            ArSession_setCameraTextureName(xrContext->Session, static_cast<uint32_t>(cameraTextureId));
           // Set Earth object
            ArSession_acquireEarth(xrContext->Session, &xrContext->Earth);

            // Start the ArSession
            {
                ArStatus status{ ArSession_resume(xrContext->Session) };
                if (status != ArStatus::AR_SUCCESS)
                {
                    std::ostringstream message;
                    message << "Failed to start ArSession with status: " << status;
                    throw std::runtime_error{ message.str() };
                }
            }

            xrContext->Initialized = true;
        }

        std::unique_ptr<Session::Frame> GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession, std::function<arcana::task<void, std::exception_ptr>(void*)> deletedTextureAsyncCallback)
        {
            if (!xrContext->Initialized)
            {
                Initialize();
            }

            ANativeWindow* activeWindow{ windowProvider() };
            if (activeWindow != window)
            {
                window = activeWindow;

                if (window)
                {
                    surface = EGLSurfacePtr(eglCreateWindowSurface(display, config, window, nullptr), [display{display}](EGLSurface surface) {
                        eglDestroySurface(display, surface);
                    });
                }
            }

            shouldEndSession = sessionEnded;
            shouldRestartSession = false;

            // Update the ArSession to get a new frame
            // ARCore needs a valid bound OpenGL context to do some offscreen rendering.
            // For some reason, ARCore destroys the surface when it's bound and activity changes.
            // To not make ARCore aware of our surface, simply don't bind it.
            {
                auto surfaceTransaction{GLTransactions::MakeCurrent(eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_NO_SURFACE, EGL_NO_SURFACE, eglGetCurrentContext())};
                ArSession_update(xrContext->Session, xrContext->Frame);
            }

            ArCamera* camera{};
            ArFrame_acquireCamera(xrContext->Session, xrContext->Frame, &camera);

            {
                // Get the current pose of the device
                ArCamera_getDisplayOrientedPose(xrContext->Session, camera, cameraPose);

                // The raw pose is exactly 7 floats: 4 for the orientation quaternion, and 3 for the position vector
                float rawPose[7]{};
                ArPose_getPoseRaw(xrContext->Session, cameraPose, rawPose);

                // Set the orientation and position
                RawToPose(rawPose, ActiveFrameViews[0].Space.Pose);
            }

            // Get the current window dimensions
            size_t width{}, height{};
            if (window)
            {
                int32_t _width{ANativeWindow_getWidth(window)};
                int32_t _height{ANativeWindow_getHeight(window)};
                if (_width > 0 && _height > 0)
                {
                    width = static_cast<size_t>(_width);
                    height = static_cast<size_t>(_height);
                }
            }

            // min size for a RT is 8x8. eglQuerySurface may return a width or height of 0 which will assert in bgfx
            width = std::max(width, size_t(8));
            height = std::max(height, size_t(8));

            // Check whether the dimensions have changed
            if ((ActiveFrameViews[0].ColorTextureSize.Width != width || ActiveFrameViews[0].ColorTextureSize.Height != height) && width && height)
            {
                DestroyDisplayResources(deletedTextureAsyncCallback);

                int rotation{ GetAppContext().getSystemService<android::view::WindowManager>().getDefaultDisplay().getRotation() };

                // Update the width and height of the display with ARCore (this is used to adjust the UVs for the camera texture so we can draw a portion of the camera frame that matches the size of the UI element displaying it)
                ArSession_setDisplayGeometry(xrContext->Session, rotation, static_cast<int32_t>(width), static_cast<int32_t>(height));

                // Allocate and store the render texture
                {
                    GLuint colorTextureId{};
                    glGenTextures(1, &colorTextureId);
                    glBindTexture(GL_TEXTURE_2D, colorTextureId);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    ActiveFrameViews[0].ColorTexturePointer = reinterpret_cast<void *>(colorTextureId);
                    ActiveFrameViews[0].ColorTextureFormat = TextureFormat::RGBA8_SRGB;
                    ActiveFrameViews[0].ColorTextureSize = {width, height};
                }
                
                {
                    glBindTexture(GL_TEXTURE_2D, babylonTextureCopyId);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }

                // Allocate and store the depth texture
                {
                    GLuint depthTextureId{};
                    glGenTextures(1, &depthTextureId);
                    glBindTexture(GL_TEXTURE_2D, depthTextureId);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8_OES, width, height, 0, GL_DEPTH_STENCIL_OES, GL_UNSIGNED_INT_24_8_OES, nullptr);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    ActiveFrameViews[0].DepthTexturePointer = reinterpret_cast<void*>(depthTextureId);
                    ActiveFrameViews[0].DepthTextureFormat = TextureFormat::D24S8;
                    ActiveFrameViews[0].DepthTextureSize = {width, height};
                }

                // Bind the color and depth texture to the camera frame buffer
                glBindFramebuffer(GL_FRAMEBUFFER, cameraFrameBufferId);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(ActiveFrameViews[0].ColorTexturePointer)), 0);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(ActiveFrameViews[0].DepthTexturePointer)), 0);
            }

            int32_t geometryChanged{0};
            ArFrame_getDisplayGeometryChanged(xrContext->Session, xrContext->Frame, &geometryChanged);

            // Check whether the projection matrix needs to be updated
            if (geometryChanged || ActiveFrameViews[0].DepthNearZ != DepthNearZ || ActiveFrameViews[0].DepthFarZ != DepthFarZ)
            {
                // Get the current projection matrix
                ArCamera_getProjectionMatrix(xrContext->Session, camera, DepthNearZ, DepthFarZ, ActiveFrameViews[0].ProjectionMatrix.data());
            }

            ActiveFrameViews[0].DepthNearZ = DepthNearZ;
            ActiveFrameViews[0].DepthFarZ = DepthFarZ;

            if (geometryChanged)
            {
                // Transform the UVs for the vertex positions given the current display size
                ArFrame_transformCoordinates2d(
                    xrContext->Session, xrContext->Frame, AR_COORDINATES_2D_OPENGL_NORMALIZED_DEVICE_COORDINATES,
                    VERTEX_COUNT, VERTEX_POSITIONS, AR_COORDINATES_2D_TEXTURE_NORMALIZED, CameraFrameUVs);
            }

            ArCamera_release(camera);

            // Draw the camera texture to the Babylon render texture, but only if the session has started providing AR frames
            int64_t frameTimestamp{};
            ArFrame_getTimestamp(xrContext->Session, xrContext->Frame, &frameTimestamp);
            if (frameTimestamp)
            {
                auto stencilMaskTransaction{ GLTransactions::SetStencil(1) };

                // Bind the frame buffer
                glBindFramebuffer(GL_FRAMEBUFFER, cameraFrameBufferId);

                // Set the viewport to the whole frame buffer
                glViewport(0, 0, width, height);

                // Disable unnecessary capabilities
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);

                // Clear the depth and stencil
                glDepthMask(GL_TRUE);
                glStencilMask(1);
                glClearDepthf(1.0);
                glClearStencil(0);
                glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

                // Only write colors to blit the background camera texture
                glDepthMask(GL_FALSE);
                glStencilMask(0);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                // Use the custom shader
                glUseProgram(cameraShaderProgramId);

                // Configure the quad vertex positions
                auto vertexPositionsUniformLocation{ glGetUniformLocation(cameraShaderProgramId, "vertexPositions") };
                glUniform2fv(vertexPositionsUniformLocation, VERTEX_COUNT, VERTEX_POSITIONS);

                // Configure the camera texture
                auto cameraTextureUniformLocation{ glGetUniformLocation(cameraShaderProgramId, "cameraTexture") };
                glUniform1i(cameraTextureUniformLocation, GetTextureUnit(GL_TEXTURE0));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, cameraTextureId);
                glBindSampler(GetTextureUnit(GL_TEXTURE0), 0);

                // Configure the camera frame UVs
                auto cameraFrameUVsUniformLocation{ glGetUniformLocation(cameraShaderProgramId, "cameraFrameUVs") };
                glUniform2fv(cameraFrameUVsUniformLocation, VERTEX_COUNT, CameraFrameUVs);

                // Draw the quad
                glDrawArrays(GL_TRIANGLE_STRIP, 0, VERTEX_COUNT);
            }

            return std::make_unique<Session::Frame>(*this);
        }

        void RequestEndSession()
        {
            // Note the end session has been requested, and respond to the request in the next call to GetNextFrame
            sessionEnded = true;

            surface.reset();
        }

        void DrawFrame()
        {
            // Draw the Babylon render texture to the display, but only if the session has started providing AR frames.
            int64_t frameTimestamp{};
            ArFrame_getTimestamp(xrContext->Session, xrContext->Frame, &frameTimestamp);
            if (frameTimestamp && surface.get())
            {
                auto surfaceTransaction{ GLTransactions::MakeCurrent(eglGetDisplay(EGL_DEFAULT_DISPLAY), surface.get(), surface.get(), context) };
                
                auto babylonTextureId{ static_cast<GLuint>(reinterpret_cast<uintptr_t>(ActiveFrameViews[0].ColorTexturePointer)) };
                
                glBindTexture(GL_TEXTURE_2D, babylonTextureId);
    
            
                // Unbind the texture
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, babylonTextureCopyFrameBufferId);

                // Attach the texture to the framebuffer
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, babylonTextureId, 0);
            
                // Unbind the framebuffer
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                
                // Bind the framebuffer that contains the source texture
                glBindFramebuffer(GL_READ_FRAMEBUFFER, babylonTextureCopyFrameBufferId);
            
                // Bind the destination texture
                glBindTexture(GL_TEXTURE_2D, babylonTextureCopyId);
            
                // Copy the texture
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, ActiveFrameViews[0].ColorTextureSize.Width, ActiveFrameViews[0].ColorTextureSize.Height);
            
                // Unbind
                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                glBindTexture(GL_TEXTURE_2D, 0);

                // COPY IS DONE
                // Now render
                
                // Bind the frame buffer
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                // Set the viewport to the whole surface
                glViewport(0, 0, ActiveFrameViews[0].ColorTextureSize.Width, ActiveFrameViews[0].ColorTextureSize.Height);

                // Disable unnecessary capabilities
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_BLEND);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_CULL_FACE);
                

                // Only write colors to blit to the screen
                //glDepthMask(GL_FALSE);
                auto stencilMaskTransaction{ GLTransactions::SetStencil(0) };
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                // Use the custom shader
                glUseProgram(babylonShaderProgramId);

                // Configure the quad vertex positions
                auto vertexPositionsUniformLocation{ glGetUniformLocation(babylonShaderProgramId, "vertexPositions") };
                glUniform2fv(vertexPositionsUniformLocation, VERTEX_COUNT, VERTEX_POSITIONS);

                // Retrieve the depth image for the current frame, if available.
                ArImage* depth_image = NULL;
                // If a depth image is available, use it here.
                if (ArFrame_acquireDepthImage16Bits(xrContext->Session, xrContext->Frame, &depth_image) ==
                    AR_SUCCESS) {
                    int image_width = 0;
                      int image_height = 0;
                      int image_pixel_stride = 0;
                      int image_row_stride = 0;
                    const uint8_t* depth_data = nullptr;
                      int plane_size_bytes = 0;
                      ArImage_getPlaneData(xrContext->Session, depth_image, /*plane_index=*/0, &depth_data,
                                           &plane_size_bytes);
                    
                      // Bails out if there's no depth_data.
                      if (depth_data != nullptr) {
                          ArImage_getWidth(xrContext->Session, depth_image, &image_width);
                          ArImage_getHeight(xrContext->Session, depth_image, &image_height);
                          ArImage_getPlanePixelStride(xrContext->Session, depth_image, 0, &image_pixel_stride);
                          ArImage_getPlaneRowStride(xrContext->Session, depth_image, 0, &image_row_stride);
                          ArImage_release(depth_image);
                          glActiveTexture(GL_TEXTURE1);
                          glBindTexture(GL_TEXTURE_2D, depthTextureId);
                          glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, image_width, image_height, 0, GL_RG,
                                       GL_UNSIGNED_BYTE, depth_data);
                            auto depthTextureUniformLocation{ glGetUniformLocation(babylonShaderProgramId, "depthTexture") };
                            glUniform1i(depthTextureUniformLocation, GetTextureUnit(GL_TEXTURE1));
                            glActiveTexture(GL_TEXTURE1);
                            
                            glBindTexture(GL_TEXTURE_2D, depthTextureId);
                            glBindSampler(GetTextureUnit(GL_TEXTURE1), 0);
                      } else {
                          ArImage_release(depth_image);
                      }
                    
                } else {
                    
                  // No depth image received for this frame.
                  // This normally means that depth data is not available yet.
                  // Depth data will not be available if there are no tracked
                  // feature points. This can happen when there is no motion, or when the
                  // camera loses its ability to track objects in the surrounding
                  // environment.
                }
                // Configure the camera texture
                auto cameraTextureUniformLocation{ glGetUniformLocation(babylonShaderProgramId, "cameraTexture") };
                glUniform1i(cameraTextureUniformLocation, GetTextureUnit(GL_TEXTURE2));
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, cameraTextureId);
                glBindSampler(GetTextureUnit(GL_TEXTURE2), 0);

                // Configure the camera frame UVs
                auto cameraFrameUVsUniformLocation{ glGetUniformLocation(babylonShaderProgramId, "cameraFrameUVs") };
                glUniform2fv(cameraFrameUVsUniformLocation, VERTEX_COUNT, CameraFrameUVs);


                auto babylonTextureCopyUniformLocation{ glGetUniformLocation(babylonShaderProgramId, "babylonTextureCopy") };
                glUniform1i(babylonTextureCopyUniformLocation, GetTextureUnit(GL_TEXTURE3));
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, babylonTextureCopyId);
                glBindSampler(GetTextureUnit(GL_TEXTURE3), 0);

                
                // Configure the babylon render texture
                auto babylonTextureUniformLocation{ glGetUniformLocation(babylonShaderProgramId, "babylonTexture") };
                glUniform1i(babylonTextureUniformLocation, GetTextureUnit(GL_TEXTURE0));
                glActiveTexture(GL_TEXTURE0);
                
                glBindTexture(GL_TEXTURE_2D, babylonTextureId);
                glBindSampler(GetTextureUnit(GL_TEXTURE0), 0);

                // Draw the quad
                glDrawArrays(GL_TRIANGLE_STRIP, 0, VERTEX_COUNT);

                // Present to the screen
                // NOTE: For a yet to be determined reason, bgfx is also doing an eglSwapBuffers when running in the Babylon Native and Babylon React Native Playground apps.
                //       The "double" eglSwapBuffers causes rendering issues, so until we figure out this issue, comment out this line while testing in the BN/BRN playground apps.
                eglSwapBuffers(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW));
            }
        }

        void GetHitTestResults(std::vector<HitResult>& filteredResults, xr::Ray offsetRay, xr::HitTestTrackableType validHitTestTypes)
        {
            if (!IsTracking())
            {
                return;
            }

            // Push the camera orientation into a glm quaternion.
            glm::quat cameraOrientationQuaternion
            {
                ActiveFrameViews[0].Space.Pose.Orientation.W,
                ActiveFrameViews[0].Space.Pose.Orientation.X,
                ActiveFrameViews[0].Space.Pose.Orientation.Y,
                ActiveFrameViews[0].Space.Pose.Orientation.Z
            };

            // Pull out the direction from the offset ray into a GLM Vector3.
            glm::vec3 direction{ offsetRay.Direction.X, offsetRay.Direction.Y, offsetRay.Direction.Z };

            // Multiply the camera rotation quaternion by the direction vector to calculate the direction vector in viewer space.
            glm::vec3 cameraOrientedDirection{cameraOrientationQuaternion * glm::normalize(direction)};
            float cameraOrientedDirectionArray[3]{ cameraOrientedDirection.x, cameraOrientedDirection.y, cameraOrientedDirection.z };

            // Convert the origin to camera space by multiplying the origin by the rotation quaternion, then adding that to the
            // position of the camera.
            glm::vec3 offsetOrigin{ offsetRay.Origin.X, offsetRay.Origin.Y, offsetRay.Origin.Z };
            offsetOrigin = cameraOrientationQuaternion * offsetOrigin;

            // Pull out the origin composited from the offsetRay and camera position into a float array.
            float hitTestOrigin[3]
            {
                ActiveFrameViews[0].Space.Pose.Position.X + offsetOrigin.x,
                ActiveFrameViews[0].Space.Pose.Position.Y + offsetOrigin.y,
                ActiveFrameViews[0].Space.Pose.Position.Z + offsetOrigin.z
            };

            // Perform a hit test and process the results.
            ArFrame_hitTestRay(xrContext->Session, xrContext->Frame, hitTestOrigin, cameraOrientedDirectionArray, hitResultList);

            // Iterate over the results and pull out only those that match the desired TrackableType.  For now we are limiting results to
            // just hits against the Plane, and further scoping that to Poses that are contained in the polygon of the detected mesh.
            // This is equivalent to XRHitTestTrackableType.mesh (https://immersive-web.github.io/hit-test/#hit-test-trackable-type-enum).
            int32_t size{};
            ArHitResultList_getSize(xrContext->Session, hitResultList, &size);
            for (int i = 0; i < size; i++)
            {
                ArTrackableType trackableType{};
                ArTrackable* trackable;

                bool hitTestResultValid{false};
                ArHitResultList_getItem(xrContext->Session, hitResultList, i, hitResult);
                ArHitResult_acquireTrackable(xrContext->Session, hitResult, &trackable);
                ArTrackable_getType(xrContext->Session, trackable, &trackableType);
                if (trackableType == AR_TRACKABLE_PLANE)
                {
                    // If we are only hit testing against planes then mark the hit test as valid otherwise check
                    // if the hit result is inside the plane mesh.
                    if ((validHitTestTypes & xr::HitTestTrackableType::PLANE) != xr::HitTestTrackableType::NONE)
                    {
                        hitTestResultValid = true;
                    }
                    else if ((validHitTestTypes & xr::HitTestTrackableType::MESH) != xr::HitTestTrackableType::NONE)
                    {
                        int32_t isPoseInPolygon{};
                        ArHitResult_getHitPose(xrContext->Session, hitResult, tempPose);
                        ArPlane_isPoseInPolygon(xrContext->Session, reinterpret_cast<ArPlane*>(trackable), tempPose, &isPoseInPolygon);
                        hitTestResultValid = isPoseInPolygon != 0;
                    }
                }
                else if (trackableType == AR_TRACKABLE_POINT && (validHitTestTypes & xr::HitTestTrackableType::POINT) != xr::HitTestTrackableType::NONE)
                {
                    // Hit a feature point, which is valid for this hit test source.
                    hitTestResultValid = true;
                }

                if (hitTestResultValid)
                {
                    float rawPose[7]{};
                    ArHitResult_getHitPose(xrContext->Session, hitResult, tempPose);
                    ArPose_getPoseRaw(xrContext->Session, tempPose, rawPose);
                    HitResult hitTestResult{};
                    RawToPose(rawPose, hitTestResult.Pose);

                    hitTestResult.NativeTrackable = reinterpret_cast<NativeTrackablePtr>(trackable);
                    filteredResults.push_back(hitTestResult);
                    frameTrackables.push_back(trackable);
                }
            }
        }

        std::vector<ImageTrackingScore>* GetImageTrackingScores()
        {
            if (imageTrackingScoresValid)
            {
                return &imageTrackingScores;
            }

            return nullptr;
        }

        void CreateAugmentedImageDatabase(const std::vector<System::Session::ImageTrackingRequest>& requests)
        {
            ArAugmentedImageDatabase_create(xrContext->Session, &augmentedImageDatabase);
            int32_t trackableImagesCount{0};

            // Loop over each image in the request, and add it to the image database.
            for (System::Session::ImageTrackingRequest image : requests)
            {
                int32_t index{0};
                ArStatus status{};
                std::vector<uint8_t> grayscaleBuffer{};
                if (image.width != image.stride)
                {
                    grayscaleBuffer.reserve(image.width * image.height);
                    ConvertBitmapToGrayscale(image.data, image.width, image.height, image.stride,
                         grayscaleBuffer.data());
                }

                // If an estimated width was provided, send that down to ARCore otherwise add the image with no size.
                if (image.measuredWidthInMeters > 0)
                {
                    status = ArAugmentedImageDatabase_addImageWithPhysicalSize(
                        xrContext->Session,
                        augmentedImageDatabase,
                        "",
                        image.width == image.stride ? image.data : grayscaleBuffer.data(),
                        image.width,
                        image.height,
                        image.width,
                        image.measuredWidthInMeters,
                        &index);
                }
                else
                {
                    status = ArAugmentedImageDatabase_addImage(
                        xrContext->Session,
                        augmentedImageDatabase,
                        "",
                        image.width == image.stride ? image.data : grayscaleBuffer.data(),
                        image.width,
                        image.height,
                        image.width,
                        &index);
                }

                if (status == AR_SUCCESS)
                {
                    trackableImagesCount++;
                    imageTrackingScores.push_back(ImageTrackingScore::TRACKABLE);
                }
                else
                {
                    imageTrackingScores.push_back(ImageTrackingScore::UNTRACKABLE);
                }
            }

            // If we had at least one trackable image, set up image tracking.
            if (trackableImagesCount > 0)
            {
                // Create an ArConfig
                ArConfig *arConfig{};
                ArConfig_create(xrContext->Session, &arConfig);
                ArSession_getConfig(xrContext->Session, arConfig);

                // Configure the ArSession to include the image tracking database
                ArConfig_setAugmentedImageDatabase(xrContext->Session, arConfig, augmentedImageDatabase);
                const ArStatus status{ArSession_configure(xrContext->Session, arConfig)};

                // If we failed to configure the session, error out.
                if (status != AR_SUCCESS)
                {
                    throw std::runtime_error{"Failed to configure AR Session for Image Tracking"};
                }

                // Clean up the ArConfig.
                ArConfig_destroy(arConfig);
            }

            imageTrackingScoresValid = true;
        }

        void UpdateImageTrackingResults(std::vector<Frame::ImageTrackingResult::Identifier>& updatedResults)
        {
            // Get list of updated images in the current frame.
            int32_t imageListSize{};
            ArFrame_getUpdatedTrackables(xrContext->Session, xrContext->Frame, AR_TRACKABLE_AUGMENTED_IMAGE, trackableList);
            ArTrackableList_getSize(xrContext->Session, trackableList, &imageListSize);

            // For each updated image, get the current status.
            for (int i{0}; i < imageListSize; ++i) {
                ArTrackable* trackable{nullptr};
                ArTrackableList_acquireItem(xrContext->Session, trackableList, i, &trackable);
                ArAugmentedImage* imageTrackable {ArAsAugmentedImage(trackable)};

                int imageIndex{};
                ArAugmentedImage_getIndex(xrContext->Session, imageTrackable, &imageIndex);

                float measuredWidthInMeters{};
                ArAugmentedImage_getExtentX(xrContext->Session, imageTrackable, &measuredWidthInMeters);
                
                float rawPose[7]{};
                ArAugmentedImage_getCenterPose(xrContext->Session, imageTrackable, tempPose);
                ArPose_getPoseRaw(xrContext->Session, tempPose, rawPose);

                ArAugmentedImageTrackingMethod trackingState{AR_AUGMENTED_IMAGE_TRACKING_METHOD_NOT_TRACKING};
                ArAugmentedImage_getTrackingMethod(xrContext->Session, imageTrackable, &trackingState);

                // Update the existing image tracking result if it exists.
                auto resultIterator{ imageTrackingResultsMap.find(imageTrackable) };
                if (resultIterator != imageTrackingResultsMap.end())
                {
                    UpdateImageTrackingResult(
                        updatedResults,
                        GetImageTrackingResultByID(resultIterator->second),
                        rawPose,
                        measuredWidthInMeters,
                        trackingState);

                    // Release reference to trackable, since we are already holding a ref count in the map.
                    ArTrackable_release(reinterpret_cast<ArTrackable*>(imageTrackable));
                }
                else
                {
                    // This is a new result, create it and initialize its values.
                    imageTrackingResults.push_back(std::make_unique<Frame::ImageTrackingResult>());
                    auto& result{ *imageTrackingResults.back() };
                    result.Index = imageIndex;
                    imageTrackingResultsMap.insert({imageTrackable, result.ID});
                    UpdateImageTrackingResult(
                        updatedResults,
                        result,
                        rawPose,
                        measuredWidthInMeters,
                        trackingState);
                }
            }
        }

        Frame::ImageTrackingResult& GetImageTrackingResultByID(Frame::ImageTrackingResult::Identifier resultID)
        {
            const auto end{imageTrackingResults.end()};
            const auto it{std::find_if(
                imageTrackingResults.begin(),
                end,
                [&](std::unique_ptr<Frame::ImageTrackingResult>& resultPtr) { return resultPtr->ID == resultID; })};

            if (it != end)
            {
                return **it;
            }
            else
            {
                throw std::runtime_error{"Tried to get non-existent image tracking result."};
            }
        }

        void UpdateImageTrackingResult(
            std::vector<Frame::ImageTrackingResult::Identifier>& updatedResults,
            Frame::ImageTrackingResult& result,
            const float rawPose[],
            float measuredWidthInMeters,
            ArAugmentedImageTrackingMethod arTrackingState)
        {
            // Update the pose and measured width
            RawToPose(rawPose, result.ImageSpace.Pose);
            result.MeasuredWidthInMeters = measuredWidthInMeters;

            // Map ARCore tracking state to WebXR image tracking state.
            result.TrackingState = arTrackingState == AR_AUGMENTED_IMAGE_TRACKING_METHOD_FULL_TRACKING
                ? ImageTrackingState::TRACKED
                : arTrackingState == AR_AUGMENTED_IMAGE_TRACKING_METHOD_LAST_KNOWN_POSE
                    ? ImageTrackingState::EMULATED
                    : ImageTrackingState::UNTRACKED;

            // Mark that this result was updated on this frame.
            updatedResults.push_back(result.ID);
        }

        // Loop over image tracking map, and release the held trackables.
        void CleanupImageTrackingTrackables()
        {
            auto imageTrackingIter{ imageTrackingResultsMap.begin() };
            while (imageTrackingIter != imageTrackingResultsMap.end())
            {
                ArTrackable_release(reinterpret_cast<ArTrackable*>(imageTrackingIter->first));
                imageTrackingIter++;
            }

            imageTrackingResultsMap.clear();
            imageTrackingResults.clear();
            imageTrackingScores.clear();
        }

        // Clean up all ArCore trackables owned by the current frame, this should be called once per frame.
        void CleanupFrameTrackables()
        {
            for (ArTrackable* trackable : frameTrackables)
            {
                ArTrackable_release(trackable);
            }

            frameTrackables.clear();
        }

        Anchor CreateAnchor(Pose pose, NativeTrackablePtr trackable)
        {
            // First translate the passed in pose to something usable by ArCore.
            ArPose* arPose{};
            float rawPose[7]{};
            PoseToRaw(rawPose, pose);
            ArPose_create(xrContext->Session, rawPose, &arPose);

            // Create the actual anchor. If a trackable was passed in (from a hit test result) create the
            // anchor against the tracakble. Otherwise create it against the session.
            ArAnchor* arAnchor{};
            auto trackableObj{ reinterpret_cast<ArTrackable*>(trackable) };
            if (trackableObj)
            {
                ArTrackable_acquireNewAnchor(xrContext->Session, trackableObj, arPose, &arAnchor);
            }
            else
            {
                ArSession_acquireNewAnchor(xrContext->Session, arPose, &arAnchor);
            }

            // Clean up the temp pose.
            ArPose_destroy(arPose);

            // Store the anchor the vector tracking currently allocated anchors, and pass back the result.
            arCoreAnchors.push_back(arAnchor);
            return { { pose }, reinterpret_cast<NativeAnchorPtr>(arAnchor) };
        }

        Anchor DeclareAnchor(NativeAnchorPtr anchor)
        {
            ArAnchor* arAnchor{reinterpret_cast<ArAnchor*>(anchor)};
            arCoreAnchors.push_back(arAnchor);

            ArPose* arPose{};
            ArAnchor_getPose(xrContext->Session, arAnchor, arPose);

            float rawPose[7]{};
            ArPose_getPoseRaw(xrContext->Session, arPose, rawPose);

            Pose pose{};
            RawToPose(rawPose, pose);

            return { { pose }, reinterpret_cast<NativeAnchorPtr>(arAnchor)};
        };

        void UpdateAnchor(xr::Anchor& anchor)
        {
            // First check if the anchor still exists, if not then mark the anchor as no longer valid.
            auto arAnchor{ reinterpret_cast<ArAnchor*>(anchor.NativeAnchor) };
            if (arAnchor == nullptr)
            {
                anchor.IsValid = false;
                return;
            }

            ArTrackingState trackingState{};
            ArAnchor_getTrackingState(xrContext->Session, arAnchor, &trackingState);

            // If tracking then update the pose, if paused then skip the update, if stopped then
            // mark this anchor as no longer valid, as it will never again be tracked by ArCore.
            if (trackingState == AR_TRACKING_STATE_TRACKING)
            {
                ArAnchor_getPose(xrContext->Session, arAnchor, tempPose);
                float rawPose[7]{};
                ArPose_getPoseRaw(xrContext->Session, tempPose, rawPose);
                RawToPose(rawPose, anchor.Space.Pose);
            }
            else if (trackingState == AR_TRACKING_STATE_STOPPED)
            {
                anchor.IsValid = false;
            }
        }

        void DeleteAnchor(xr::Anchor& anchor)
        {
            // If this anchor has not already been deleted, then detach it from the current AR session,
            // and clean up its state in memory.
            if (anchor.NativeAnchor != nullptr)
            {
                auto arAnchor{ reinterpret_cast<ArAnchor*>(anchor.NativeAnchor) };
                ArAnchor_detach(xrContext->Session, arAnchor);
                CleanupAnchor(arAnchor);
                anchor.NativeAnchor = nullptr;
            }
        }

        void CleanupAnchor(ArAnchor* arAnchor)
        {
            // Iterate over the list of anchors if arAnchor is null then clean up all anchors
            // otherwise clean up only the target anchor and return.
            auto anchorIter{ arCoreAnchors.begin() };
            while (anchorIter != arCoreAnchors.end())
            {
                if (arAnchor == nullptr || arAnchor == *anchorIter)
                {
                    ArAnchor_release(*anchorIter);
                    anchorIter = arCoreAnchors.erase(anchorIter);

                    if (arAnchor != nullptr)
                    {
                        return;
                    }
                }
                else
                {
                    anchorIter++;
                }
            }
        }

        void UpdatePlanes(std::vector<Frame::Plane::Identifier>& updatedPlanes, std::vector<Frame::Plane::Identifier>& deletedPlanes)
        {
            if (!PlaneDetectionEnabled)
            {
                return;
            }

            // First check if any existing planes have been subsumed by another plane, if so add them to the list of deleted planes
            CheckForSubsumedPlanes(deletedPlanes);

            // Next check for updated planes, and update their pose and polygon or create a new plane if it does not yet exist.
            ArFrame_getUpdatedTrackables(xrContext->Session, xrContext->Frame, AR_TRACKABLE_PLANE, trackableList);
            int32_t size{};
            ArTrackableList_getSize(xrContext->Session, trackableList, &size);
            for (int i = 0; i < size; i++)
            {
                // Get the plane.
                ArPlane* planeTrackable{};
                {
                    ArTrackable* trackable{};
                    ArTrackableList_acquireItem(xrContext->Session, trackableList, i, &trackable);
                    planeTrackable = reinterpret_cast<ArPlane*>(trackable);
                }

                // Check if this plane has been subsumed. If so skip it as we are about to delete this plane.
                ArPlane* subsumingPlane{};
                ArPlane_acquireSubsumedBy(xrContext->Session, planeTrackable, &subsumingPlane);
                if (subsumingPlane != nullptr)
                {
                    ArTrackable_release(reinterpret_cast<ArTrackable*>(planeTrackable));
                    ArTrackable_release(reinterpret_cast<ArTrackable*>(subsumingPlane));
                    continue;
                }

                // Get the center pose.
                float rawPose[7]{};
                ArPlane_getCenterPose(xrContext->Session, planeTrackable, tempPose);
                ArPose_getPoseRaw(xrContext->Session, tempPose, rawPose);

                // Dynamically allocate the polygon vector, and fill it in.
                int32_t polygonSize;
                ArPlane_getPolygonSize(xrContext->Session, planeTrackable, &polygonSize);
                planePolygonBuffer.clear();
                planePolygonBuffer.resize(polygonSize);
                ArPlane_getPolygon(xrContext->Session, planeTrackable, planePolygonBuffer.data());

                // Update the existing plane if it exists, otherwise create a new plane, and add it to our list of planes.
                auto planeIterator{ planeMap.find(planeTrackable) };
                if (planeIterator != planeMap.end())
                {
                    UpdatePlane(updatedPlanes, GetPlaneByID(planeIterator->second), rawPose, planePolygonBuffer, polygonSize);
                    ArTrackable_release(reinterpret_cast<ArTrackable*>(planeTrackable));
                }
                else
                {
                    // This is a new plane, create it and initialize its values.
                    Planes.emplace_back();
                    auto& plane{ Planes.back() };
                    planeMap.insert({planeTrackable, plane.ID});
                    UpdatePlane(updatedPlanes, plane, rawPose, planePolygonBuffer, polygonSize);
                }
            }
        }

        void UpdateFeaturePointCloud()
        {
            if (!FeaturePointCloudEnabled)
            {
                return;
            }

            // Get the feature point cloud from ArCore.
            ArPointCloud *pointCloud{};
            int32_t numberOfPoints{};
            const int32_t* pointCloudIDs{};
            const float *pointCloudData{};
            ArStatus status{ ArFrame_acquirePointCloud(xrContext->Session, xrContext->Frame, &pointCloud) };

            if (status != AR_SUCCESS)
            {
                FeaturePointCloud.clear();
                return;
            }

            try
            {
                ArPointCloud_getNumberOfPoints(xrContext->Session, pointCloud, &numberOfPoints);
                ArPointCloud_getData(xrContext->Session, pointCloud, &pointCloudData);
                ArPointCloud_getPointIds(xrContext->Session, pointCloud, &pointCloudIDs);

                FeaturePointCloud.resize(numberOfPoints);
                for (int32_t i = 0; i < numberOfPoints; i++)
                {
                    FeaturePointCloud.emplace_back();
                    auto& featurePoint{ FeaturePointCloud.back() };
                    int32_t dataIndex{ i * 4 };

                    // Grab the position and confidence value from the point cloud.
                    // Reflect the point across the Z axis, as we want to report this
                    // value in camera space.
                    featurePoint.X = pointCloudData[dataIndex];
                    featurePoint.Y = pointCloudData[dataIndex + 1];
                    featurePoint.Z = -1 * pointCloudData[dataIndex + 2];
                    featurePoint.ConfidenceValue = pointCloudData[dataIndex + 3];

                    // Check to see if this point ID exists in our point cloud mapping if not add it to the map.
                    const int32_t id{ pointCloudIDs[i] };
                    auto featurePointIterator = featurePointIDMap.find(id);
                    if (featurePointIterator != featurePointIDMap.end())
                    {
                        featurePoint.ID = featurePointIterator->second;
                    }
                    else
                    {
                        featurePoint.ID = nextFeaturePointID++;
                        featurePointIDMap.insert({id, featurePoint.ID});
                    }
                }
            }
            catch (std::exception)
            {
                // Release the point cloud to free its memory.
                ArPointCloud_release(pointCloud);
                throw;
            }

            // Release the point cloud to free its memory.
            ArPointCloud_release(pointCloud);
        }

        Frame::Plane& GetPlaneByID(Frame::Plane::Identifier planeID)
        {
            const auto end{Planes.end()};
            const auto it{std::find_if(Planes.begin(), end, [&](Frame::Plane& plane) { return plane.ID == planeID; })};

            if (it != end)
            {
                return *it;
            }
            else
            {
                throw std::runtime_error{"Tried to get non-existent plane."};
            }
        }

        /**
         * Checks whether the AR camera is currently tracking.
         **/
        bool IsTracking()
        {
            ArCamera* camera{};
            ArTrackingState trackingState{};
            ArFrame_acquireCamera(xrContext->Session, xrContext->Frame, &camera);
            ArCamera_getTrackingState(xrContext->Session, camera, &trackingState);
            return trackingState == ArTrackingState::AR_TRACKING_STATE_TRACKING;
        }

    private:
        std::shared_ptr<XrContextARCore> xrContext{nullptr};
        bool sessionEnded{false};
        std::vector<ArTrackable*> frameTrackables{};
        std::vector<ArAnchor*> arCoreAnchors{};
        std::vector<float> planePolygonBuffer{};
        std::unordered_map<ArPlane*, Frame::Plane::Identifier> planeMap{};
        std::unordered_map<int32_t, FeaturePoint::Identifier> featurePointIDMap{};
        FeaturePoint::Identifier nextFeaturePointID{};

        bool imageTrackingScoresValid{false};
        std::vector<ImageTrackingScore> imageTrackingScores{};
        std::vector<std::unique_ptr<Frame::ImageTrackingResult>> imageTrackingResults{};
        std::unordered_map<ArAugmentedImage*, Frame::ImageTrackingResult::Identifier> imageTrackingResultsMap{};

        std::function<ANativeWindow*()> windowProvider{};
        ANativeWindow* window{};
        EGLDisplay display{};
        EGLConfig config{};
        EGLint format{};
        EGLContext context{};
        EGLSurfacePtr surface;

        GLuint cameraShaderProgramId{};
        GLuint babylonShaderProgramId{};
        GLuint cameraTextureId{};
        GLuint depthTextureId{};
        GLuint babylonTextureCopyId{};
        GLuint babylonTextureCopyFrameBufferId{};
        GLuint cameraFrameBufferId{};

        ArPose* cameraPose{};
        ArPose* tempPose{};
        ArHitResultList* hitResultList{};
        ArHitResult* hitResult{};
        ArTrackableList* trackableList{};
        ArAugmentedImageDatabase* augmentedImageDatabase{nullptr};

        float CameraFrameUVs[VERTEX_COUNT * 2]{};

        AppStateChangedCallbackTicket pauseTicket;
        AppStateChangedCallbackTicket resumeTicket;

        void PauseSession()
        {
            if (xrContext->Session)
            {
                ArSession_pause(xrContext->Session);
            }
        }

        void ResumeSession()
        {
            if (xrContext->Session)
            {
                ArSession_resume(xrContext->Session);
            }
        }

        void DestroyDisplayResources(std::function<arcana::task<void, std::exception_ptr>(void*)> deletedTextureAsyncCallback = [](void*){ return arcana::task_from_result<std::exception_ptr>(); })
        {
            if (ActiveFrameViews[0].ColorTexturePointer != nullptr && ActiveFrameViews[0].DepthTexturePointer != nullptr) {
                auto colorTextureId{ static_cast<GLuint>(reinterpret_cast<uintptr_t>(ActiveFrameViews[0].ColorTexturePointer)) };
                auto depthTextureId{ static_cast<GLuint>(reinterpret_cast<uintptr_t>(ActiveFrameViews[0].DepthTexturePointer)) };
                deletedTextureAsyncCallback(ActiveFrameViews[0].ColorTexturePointer).then(arcana::inline_scheduler, arcana::cancellation::none(), [colorTextureId, depthTextureId]() {
                    glDeleteTextures(1, &colorTextureId);
                    glDeleteTextures(1, &depthTextureId);
                });
            }

            ActiveFrameViews[0] = {};
        }

        void PoseToRaw(float rawPose[], const Pose& pose)
        {
            rawPose[0] = pose.Orientation.X;
            rawPose[1] = pose.Orientation.Y;
            rawPose[2] = pose.Orientation.Z;
            rawPose[3] = pose.Orientation.W;
            rawPose[4] = pose.Position.X;
            rawPose[5] = pose.Position.Y;
            rawPose[6] = pose.Position.Z;
        }

        void RawToPose(const float rawPose[], Pose& pose)
        {
            pose.Orientation.X = rawPose[0];
            pose.Orientation.Y = rawPose[1];
            pose.Orientation.Z = rawPose[2];
            pose.Orientation.W = rawPose[3];
            pose.Position.X = rawPose[4];
            pose.Position.Y = rawPose[5];
            pose.Position.Z = rawPose[6];
        }

        /**
         * Checks whether this plane has been subsumed (i.e. no longer needed), and adds it to the vector if so.
         **/
        void CheckForSubsumedPlanes(std::vector<Frame::Plane::Identifier>& subsumedPlanes)
        {
            auto planeMapIterator{ planeMap.begin() };
            while (planeMapIterator != planeMap.end())
            {
                auto [arPlane, planeID]{ *planeMapIterator };

                // Check if the plane has been subsumed, and if we should stop tracking it.
                ArPlane* subsumingPlane{};
                ArPlane_acquireSubsumedBy(xrContext->Session, arPlane, &subsumingPlane);

                // Plane has been subsumed, stop tracking it explicitly.
                if (subsumingPlane != nullptr)
                {
                    subsumedPlanes.push_back(planeID);

                    auto& plane{ GetPlaneByID(planeID) };
                    plane.Polygon.clear();
                    plane.PolygonSize = 0;

                    planeMapIterator = planeMap.erase(planeMapIterator);
                    ArTrackable_release(reinterpret_cast<ArTrackable*>(arPlane));
                    ArTrackable_release(reinterpret_cast<ArTrackable*>(subsumingPlane));
                }
                else
                {
                    planeMapIterator++;
                }
            }
        }

        void UpdatePlane(std::vector<Frame::Plane::Identifier>& updatedPlanes, Frame::Plane& plane, const float rawPose[], std::vector<float>& newPolygon, size_t polygonSize)
        {
            // Grab the new center
            Pose newCenter{};
            RawToPose(rawPose, newCenter);

            // Plane was not actually updated return.
            if (!CheckIfPlaneWasUpdated(plane, newPolygon, newCenter))
            {
                return;
            }

            // Update the center pose.
            plane.Center = newCenter;

            // Swap the old polygon with the new one.
            plane.Polygon.swap(newPolygon);

            // Set the polygon size, and format.
            plane.PolygonSize = polygonSize / 2;
            plane.PolygonFormat = PolygonFormat::XZ;
            updatedPlanes.push_back(plane.ID);
        }
    };

    struct System::Session::Frame::Impl
    {
        Impl(Session::Impl& sessionImpl)
            : sessionImpl{sessionImpl}
        {
        }

        Session::Impl& sessionImpl;
    };

    System::Session::Frame::Frame(Session::Impl& sessionImpl)
        : Views{ sessionImpl.ActiveFrameViews }
        , InputSources{ sessionImpl.InputSources }
        , FeaturePointCloud{ sessionImpl.FeaturePointCloud }
        , EyeTrackerSpace{ sessionImpl.EyeTrackerSpace }
        , UpdatedSceneObjects{}
        , RemovedSceneObjects{}
        , UpdatedPlanes{}
        , RemovedPlanes{}
        , UpdatedMeshes{}
        , RemovedMeshes{}
        , UpdatedImageTrackingResults{}
        , IsTracking{sessionImpl.IsTracking()}
        , m_impl{ std::make_unique<Session::Frame::Impl>(sessionImpl) }
    {
        if (IsTracking)
        {
            m_impl->sessionImpl.UpdatePlanes(UpdatedPlanes, RemovedPlanes);
            m_impl->sessionImpl.UpdateFeaturePointCloud();
            m_impl->sessionImpl.UpdateImageTrackingResults(UpdatedImageTrackingResults);
        }
    }

    void System::Session::Frame::GetHitTestResults(std::vector<HitResult>& filteredResults, xr::Ray offsetRay, xr::HitTestTrackableType trackableTypes) const
    {
        m_impl->sessionImpl.GetHitTestResults(filteredResults, offsetRay, trackableTypes);
    }

    Anchor System::Session::Frame::CreateAnchor(Pose pose, NativeTrackablePtr trackable) const
    {
        return m_impl->sessionImpl.CreateAnchor(pose, trackable);
    }

    Anchor System::Session::Frame::DeclareAnchor(NativeAnchorPtr anchor) const
    {
        return m_impl->sessionImpl.DeclareAnchor(anchor);
    }

    void System::Session::Frame::UpdateAnchor(xr::Anchor& anchor) const
    {
        m_impl->sessionImpl.UpdateAnchor(anchor);
    }

    void System::Session::Frame::DeleteAnchor(xr::Anchor& anchor) const
    {
        m_impl->sessionImpl.DeleteAnchor(anchor);
    }

    System::Session::Frame::SceneObject& System::Session::Frame::GetSceneObjectByID(System::Session::Frame::SceneObject::Identifier /*sceneObjectID*/) const
    {
        throw std::runtime_error{"Scene object detection is not supported on current platform."};
    }

    System::Session::Frame::Plane& System::Session::Frame::GetPlaneByID(System::Session::Frame::Plane::Identifier planeID) const
    {
        return m_impl->sessionImpl.GetPlaneByID(planeID);
    }

    System::Session::Frame::Mesh& System::Session::Frame::GetMeshByID(System::Session::Frame::Mesh::Identifier /*meshID*/) const
    {
        throw std::runtime_error{"Mesh detection is not supported on current platform."};
    }

    System::Session::Frame::ImageTrackingResult& System::Session::Frame::GetImageTrackingResultByID(System::Session::Frame::ImageTrackingResult::Identifier imageResultID) const
    {
        return m_impl->sessionImpl.GetImageTrackingResultByID(imageResultID);
    }

    
    System::Session::Frame::~Frame()
    {
        m_impl->sessionImpl.CleanupFrameTrackables();
        m_impl->sessionImpl.DrawFrame();
    }

    System::System(const char* appName)
        : m_impl{ std::make_unique<System::Impl>(appName) }
    {}

    System::~System() {}

    bool System::IsInitialized() const
    {
        return m_impl->IsInitialized();
    }

    bool System::TryInitialize()
    {
        return m_impl->TryInitialize();
    }

    arcana::task<bool, std::exception_ptr> System::IsSessionSupportedAsync(SessionType sessionType)
    {
        // Currently only AR is supported on Android
        if (sessionType == SessionType::IMMERSIVE_AR)
        {
            // Spin up a background thread to own the polling check.
            arcana::task_completion_source<bool, std::exception_ptr> tcs;
            std::thread([tcs]() mutable
            {
                // Query ARCore to check if AR sessions are supported.
                // If not yet installed then poll supported status up to 100 times over 20 seconds.
                for (int i = 0; i < 100; i++)
                {
                    ArAvailability arAvailability{};
                    ArCoreApk_checkAvailability(GetEnvForCurrentThread(), GetAppContext(), &arAvailability);
                    switch (arAvailability)
                    {
                        case AR_AVAILABILITY_SUPPORTED_APK_TOO_OLD:
                        case AR_AVAILABILITY_SUPPORTED_INSTALLED:
                        case AR_AVAILABILITY_SUPPORTED_NOT_INSTALLED:
                            tcs.complete(true);
                            break;
                        case AR_AVAILABILITY_UNKNOWN_CHECKING:
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            break;
                        default:
                            tcs.complete(false);
                            break;
                    }

                    if (tcs.completed())
                    {
                        break;
                    }
                }

                if (!tcs.completed())
                {
                    tcs.complete(false);
                }
            }).detach();

            return tcs.as_task();
        }

        // VR and inline sessions are not supported at this time.
        return arcana::task_from_result<std::exception_ptr>(false);
    }

    uintptr_t System::GetNativeXrContext()
    {
        return reinterpret_cast<uintptr_t>(m_impl->XrContext.get());
    }

    std::string System::GetNativeXrContextType()
    {
        return "ARCore";
    }
    void System::removeEarthAnchor(std::string anchor_name) {
        std::shared_ptr<ArAnchor*> ar_anchor;
        auto jt = m_impl->XrContext->EarthAnchors.find(anchor_name);
        if (jt != m_impl->XrContext->EarthAnchors.end()) {
            ar_anchor = jt->second;
            ArAnchor_detach(m_impl->XrContext->Session, *ar_anchor);
            ArAnchor_release(*ar_anchor);
            m_impl->XrContext->EarthAnchors.erase(anchor_name);
        }
    }

    void System::getEarthAnchorPose(std::string anchor_name, float *out_matrix, float *out_cam_matrix) {
        std::shared_ptr<ArAnchor*> ar_anchor;
        auto jt = m_impl->XrContext->EarthAnchors.find(anchor_name);
        if (jt != m_impl->XrContext->EarthAnchors.end()) {
            ar_anchor = jt->second;
            ArPose *out_pose = NULL;
            ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
            ArAnchor_getPose(m_impl->XrContext->Session, *ar_anchor, out_pose);
            if (out_pose == NULL) return;
            ArCamera* out_camera = NULL;
            ArFrame_acquireCamera(
                    m_impl->XrContext->Session,
                    m_impl->XrContext->Frame,
                    &out_camera
            );
            if (out_camera == NULL) return;
            ArPose *out_cam_pose = NULL;
            ArPose_create(m_impl->XrContext->Session, NULL, &out_cam_pose);
            ArCamera_getDisplayOrientedPose(
                    m_impl->XrContext->Session,
                    out_camera,
                    out_cam_pose
            );
            if(out_cam_pose == NULL) return;
            ArPose_getMatrix(m_impl->XrContext->Session, out_pose, out_matrix);
            ArPose_getMatrix(m_impl->XrContext->Session, out_cam_pose, out_cam_matrix);
            ArPose_destroy(out_pose);
            ArPose_destroy(out_cam_pose);
        } else {
            std::shared_ptr<ArResolveCloudAnchorFuture*> ar_future;
            auto jt2 = m_impl->XrContext->CloudAnchorResolvingFutures.find(anchor_name);
            if (jt2 != m_impl->XrContext->CloudAnchorResolvingFutures.end()) {
                //LOGD("Cloud resolving future!\n");
                ar_future = jt2->second;
                ArFutureState ar_future_state;
                //LOGD("GTAP future bef get state!\n");
                ArFuture_getState(m_impl->XrContext->Session, reinterpret_cast<ArFuture*>(*ar_future), &ar_future_state);
                //LOGD("GTAP future af get state!\n");
                //LOGD("GTAP future cloud state is %d\n", ar_future_state);
                if (ar_future_state == AR_FUTURE_STATE_DONE) {
                    ArAnchor* earth_anchor = NULL;
                    //LOGD("GTAP future bef acq result anchor!\n");
                    ArCloudAnchorState cloud_anchor_state;
                    ArResolveCloudAnchorFuture_getResultCloudAnchorState(m_impl->XrContext->Session,
                                                                           *ar_future, &cloud_anchor_state);
                    //LOGD("GTAP future bef cloud anchor state!\n");
                    //LOGD("GTAP cloud anchor state: %d\n", cloud_anchor_state);
                    if (cloud_anchor_state == AR_CLOUD_ANCHOR_STATE_SUCCESS) {
                        ArResolveCloudAnchorFuture_acquireResultAnchor(m_impl->XrContext->Session,
                                                                       *ar_future, &earth_anchor);
                        //LOGD("GTAP future af acq result anchor!\n");
                        auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
                        m_impl->XrContext->EarthAnchors[anchor_name] = std::move(earth_anchor_ptr);
                        //LOGD("GTAP future af emplace!\n");
                        ArTrackingState tracking_state;
                        //LOGD("GTAP future bef get tracking state!\n");
                        ArAnchor_getTrackingState(m_impl->XrContext->Session, earth_anchor, &tracking_state);
                        //LOGD("GTAP future af get tracking state!\n");
                        if (tracking_state == AR_TRACKING_STATE_TRACKING) {
                            //LOGD("GTAP future af get tracking state, State is tracking!\n");
                            ArPose *out_pose = NULL;
                            ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
                            ArAnchor_getPose(m_impl->XrContext->Session, earth_anchor, out_pose);
                            ArPose_getMatrix(m_impl->XrContext->Session, out_pose, out_matrix);
                        }
                        ArFuture_release(reinterpret_cast<ArFuture*>(*ar_future));
                        m_impl->XrContext->CloudAnchorResolvingFutures.erase(anchor_name);
                    }
                }
            }
        }
    }

    void System::getCloudAnchorHostStatus(std::string anchor_name, std::string *out_cloud_anchor_id, bool *out_hosted, bool *out_error) {
        std::shared_ptr<ArHostCloudAnchorFuture*> ar_future;
        auto jt2 = m_impl->XrContext->CloudAnchorHostingFutures.find(anchor_name);
        if (jt2 != m_impl->XrContext->CloudAnchorHostingFutures.end()) {
            //LOGD("Cloud Hosting future!\n");
            ar_future = jt2->second;
            ArFutureState ar_future_state;
            //LOGD("GTAP future bef get state!\n");
            ArFuture_getState(m_impl->XrContext->Session, reinterpret_cast<ArFuture*>(*ar_future), &ar_future_state);
            //LOGD("GTAP future af get state!\n");
            if (ar_future_state == AR_FUTURE_STATE_DONE) {
                //LOGD("GTAP future bef acq result anchor!\n");
                char* out_cloud_anchor_id_char = NULL;
                ArHostCloudAnchorFuture_acquireResultCloudAnchorId(
                        m_impl->XrContext->Session,
                        *ar_future,
                        &out_cloud_anchor_id_char
                );
                //LOGD("GTAP future af acq result anchor!\n");
                if (out_cloud_anchor_id_char == NULL) return;
                //LOGD("GTAP future result anchor id not null!\n");
                *out_cloud_anchor_id = std::string(out_cloud_anchor_id_char);

                *out_error = false;
                *out_hosted = true;
                ArFuture_release(reinterpret_cast<ArFuture*>(*ar_future));
                m_impl->XrContext->CloudAnchorHostingFutures.erase(anchor_name);
            } else if (ar_future_state == AR_FUTURE_STATE_PENDING){
                *out_error = false;
            } else {
                ArFuture_release(reinterpret_cast<ArFuture*>(*ar_future));
                m_impl->XrContext->CloudAnchorHostingFutures.erase(anchor_name);
            }
            return;
        }
    }

    void System::getEarthAnchorGeospatialPose(std::string anchor_name, float *out_eus_quaternion4, double *out_latitude, double *out_longitude, double *out_altitude, bool *out_success, double *out_horizontal_accuracy, double *out_orientation_yaw_accuracy_degrees, double *out_vertical_accuracy) {
        std::shared_ptr<ArAnchor*> ar_anchor;
        auto jt = m_impl->XrContext->EarthAnchors.find(anchor_name);
        if (jt != m_impl->XrContext->EarthAnchors.end()) {
            ar_anchor = jt->second;
            ArPose *out_pose = NULL;
            ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
            ArAnchor_getPose(m_impl->XrContext->Session, *ar_anchor, out_pose);
            ArGeospatialPose* out_ar_geospatial_pose = NULL;
            ArGeospatialPose_create(m_impl->XrContext->Session, &out_ar_geospatial_pose);
            auto ar_status = ArEarth_getGeospatialPose(m_impl->XrContext->Session, m_impl->XrContext->Earth, out_pose, out_ar_geospatial_pose);
            if (ar_status != AR_SUCCESS) return;
            ArGeospatialPose_getEastUpSouthQuaternion(m_impl->XrContext->Session, out_ar_geospatial_pose, out_eus_quaternion4);
            ArGeospatialPose_getAltitude(m_impl->XrContext->Session, out_ar_geospatial_pose, out_altitude);
            ArGeospatialPose_getLatitudeLongitude(m_impl->XrContext->Session, out_ar_geospatial_pose, out_latitude, out_longitude);

            ArGeospatialPose_getHorizontalAccuracy(m_impl->XrContext->Session, out_ar_geospatial_pose, out_horizontal_accuracy);
            ArGeospatialPose_getOrientationYawAccuracy(m_impl->XrContext->Session, out_ar_geospatial_pose, out_orientation_yaw_accuracy_degrees);
            ArGeospatialPose_getVerticalAccuracy(m_impl->XrContext->Session, out_ar_geospatial_pose, out_vertical_accuracy);

            *out_success = true;
        }
    }

    void System::addLocalEarthAnchor(std::string anchor_name, float *in_quaternion4_translation3, bool *out_placed, float *out_quaternion_4, double *out_altitude, double *out_latitude, double *out_longitude) {
        //LOGD("LEA in func\n");
        if (m_impl->XrContext->Earth == NULL) return;
        ArPose* out_pose;
        //LOGD("LEA after out pose\n");
        ArPose_create(
                m_impl->XrContext->Session,
                in_quaternion4_translation3,
                &out_pose
        );
        //LOGD("LEA after create\n");
        //TODO: add ArPose_destroy
        if (out_pose == NULL) return;
        //LOGD("LEA after check out pose\n");
        ArGeospatialPose* out_geospatial_pose = NULL;
        ArGeospatialPose_create(m_impl->XrContext->Session, &out_geospatial_pose);
        //LOGD("LEA after geo create\n");
        ArTrackingState earth_tracking_state = AR_TRACKING_STATE_STOPPED;
        ArTrackable_getTrackingState(m_impl->XrContext->Session, (ArTrackable*)m_impl->XrContext->Earth,
                                     &earth_tracking_state);
        //LOGD("LEA after ts test\n");
        if (earth_tracking_state != AR_TRACKING_STATE_TRACKING) return;
        ArStatus tracking_status = AR_ERROR_NOT_TRACKING;
        tracking_status = ArEarth_getGeospatialPose(
                m_impl->XrContext->Session,
                m_impl->XrContext->Earth,
                out_pose,
                out_geospatial_pose
        );
        //LOGD("LEA after status test\n");
        if (tracking_status != AR_SUCCESS || out_geospatial_pose == NULL) return;
        //LOGD("LEA quat methods\n");
        ArGeospatialPose_getEastUpSouthQuaternion(m_impl->XrContext->Session, out_geospatial_pose, out_quaternion_4);

        ArGeospatialPose_getAltitude(m_impl->XrContext->Session, out_geospatial_pose, out_altitude);

        ArGeospatialPose_getLatitudeLongitude(m_impl->XrContext->Session, out_geospatial_pose, out_latitude, out_longitude);
        //LOGD("LEA before add earth anchor\n");
        addEarthAnchor(anchor_name, out_quaternion_4, *out_latitude, *out_longitude, *out_altitude, out_placed);
    }

    void System::resolveCloudAnchor(std::string anchor_name, std::string cloud_anchor_id, bool *out_error) {
        ArResolveCloudAnchorFuture *ar_future = NULL;
        auto anchor_status = ArSession_resolveCloudAnchorAsync(
                m_impl->XrContext->Session,
                cloud_anchor_id.c_str(),
                NULL,
                [](void *context, ArAnchor *anchor, ArCloudAnchorState cloud_anchor_state) {
                    (void) context;
                    (void) anchor;
                    (void) cloud_anchor_state;
                    //LOGD("In cloud anchor resolve callback");
                    },
                    &ar_future
        );

        if (ar_future == NULL) return;
        auto future_ptr = std::make_shared<ArResolveCloudAnchorFuture*>(ar_future);
        //auto future_ptr = std::make_shared<ArFuture*>(ar_future);
        m_impl->XrContext->CloudAnchorResolvingFutures[anchor_name] = std::move(future_ptr);
        // This anchor can't be used immediately; check its ArTrackingState
        // and ArTerrainAnchorState before rendering content on this anchor.
        if (anchor_status != AR_SUCCESS) return;
        //LOGD("AEA anchor pre-save\n");
        //if (earth_anchor == NULL) return;
        //LOGD("AEA anchor save e_anchor not null\n");
        //auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
        //m_impl->XrContext->EarthAnchors.emplace(anchor_name, std::move(earth_anchor_ptr));
        *out_error = false;
    }

    void System::hostCloudAnchor(std::string anchor_name, int in_ttl_days, bool *out_error) {
        if (m_impl->XrContext->Earth != NULL) {
            ArTrackingState earth_tracking_state = AR_TRACKING_STATE_STOPPED;
            ArTrackable_getTrackingState(m_impl->XrContext->Session, (ArTrackable*)m_impl->XrContext->Earth,
                                         &earth_tracking_state);
            if (earth_tracking_state == AR_TRACKING_STATE_TRACKING) {
                //ArAnchor* earth_anchor = NULL;
                ArHostCloudAnchorFuture* ar_future = NULL;
                std::shared_ptr<ArAnchor*> ar_anchor;
                auto jt = m_impl->XrContext->EarthAnchors.find(anchor_name);
                if (jt != m_impl->XrContext->EarthAnchors.end()) {
                    ar_anchor = jt->second;
                    //LOGD("Add Cloud Anchor in anchor name! %s\n", anchor_name.c_str());
                    auto anchor_status = ArSession_hostCloudAnchorAsync(
                            m_impl->XrContext->Session,
                            *ar_anchor,
                            in_ttl_days,
                            NULL, [](void *context, char *cloud_anchor_id, ArCloudAnchorState cloud_anchor_state) {
                                (void) context;
                                (void) cloud_anchor_id;
                                (void) cloud_anchor_state;
                                //LOGD("Add Cloud Anchor in cb!\n");
                            }, &ar_future);

                    if (ar_future == NULL) return;
                    auto future_ptr = std::make_shared<ArHostCloudAnchorFuture *>(ar_future);
                    //auto future_ptr = std::make_shared<ArFuture*>(ar_future);
                    m_impl->XrContext->CloudAnchorHostingFutures[anchor_name] = std::move(future_ptr);
                    // This anchor can't be used immediately; check its ArTrackingState
                    // and ArTerrainAnchorState before rendering content on this anchor.
                    if (anchor_status != AR_SUCCESS) return;
                    //LOGD("AEA anchor pre-save\n");
                    //if (earth_anchor == NULL) return;
                    //LOGD("AEA anchor save e_anchor not null\n");
                    //auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
                    //m_impl->XrContext->EarthAnchors.emplace(anchor_name, std::move(earth_anchor_ptr));
                    *out_error = false;
                }
            }
        }
    }



    void System::estimateFeatureMapQualityForHosting(std::string anchor_name, bool *is_good) {
        std::shared_ptr<ArAnchor*> ar_anchor;
        auto jt = m_impl->XrContext->EarthAnchors.find(anchor_name);
        if (jt != m_impl->XrContext->EarthAnchors.end()) {
            ar_anchor = jt->second;
            ArPose *out_pose = NULL;
            ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
            ArAnchor_getPose(m_impl->XrContext->Session, *ar_anchor, out_pose);
            if (out_pose == NULL) return;
            ArFeatureMapQuality out_feature_map_quality;
            auto estimation_status = ArSession_estimateFeatureMapQualityForHosting(
                    m_impl->XrContext->Session,
                    out_pose,
                    &out_feature_map_quality
            );
            if (estimation_status != AR_SUCCESS) return;
            if (out_feature_map_quality == AR_FEATURE_MAP_QUALITY_INSUFFICIENT) {
                LOGD("AFMQ insufficient!\n");
            }
            if (out_feature_map_quality == AR_FEATURE_MAP_QUALITY_SUFFICIENT) {
                LOGD("AFMQ sufficient!\n");
            }
            if (out_feature_map_quality == AR_FEATURE_MAP_QUALITY_GOOD) {
                LOGD("AFMQ good!\n");
            }
            *is_good = out_feature_map_quality != AR_FEATURE_MAP_QUALITY_INSUFFICIENT;
        }
    }

    void System::hitTestAnchor(std::string anchor_name, float in_tap_x, float in_tap_y, bool *out_placed) {
        if (m_impl->XrContext->Earth == NULL) return;
        //LOGD("ARG in hit test anchor\n");
        ArHitResultList* hit_result_list = NULL;
        ArHitResultList_create(m_impl->XrContext->Session, &hit_result_list);
        //LOGD("ARG after hit test result list, x=%.2f, y=%.2f", in_tap_x, in_tap_y);
        ArFrame_hitTest(m_impl->XrContext->Session, m_impl->XrContext->Frame, in_tap_x, in_tap_y, hit_result_list);
        //LOGD("ARG after ar frame hit test\n");
        int32_t hit_result_list_size = 0;
        ArHitResultList_getSize(m_impl->XrContext->Session, hit_result_list, &hit_result_list_size);
        //LOGD("ARG after hit result get size\n");
        // Returned hit-test results are sorted by increasing distance from the camera
        // or virtual ray's origin. The first hit result is often the most relevant
        // when responding to user input.
        ArHitResult* ar_hit = NULL;
        //LOGD("ARG before loop iter\n");
        for (int32_t i = 0; i < hit_result_list_size; i++) {
            //ArHitResult* ar_hit = NULL;
            ArHitResult_create(m_impl->XrContext->Session, &ar_hit);
            //LOGD("ARG after hit result create\n");
            ArHitResultList_getItem(m_impl->XrContext->Session, hit_result_list, i, ar_hit);
            //LOGD("ARG after hit get item\n");
            if (ar_hit == NULL) {
                //LOGE("No item was hit.");
                return;
            }

            ArTrackable* ar_trackable = NULL;
            ArHitResult_acquireTrackable(m_impl->XrContext->Session, ar_hit, &ar_trackable);
            ArTrackableType ar_trackable_type = AR_TRACKABLE_NOT_VALID;
            ArTrackable_getType(m_impl->XrContext->Session, ar_trackable, &ar_trackable_type);
            // Creates an anchor if a plane was hit.
            if (ar_trackable_type == AR_TRACKABLE_PLANE) {
                // Do something with this hit result. For example, create an anchor at
                // this point of interest.
                //ArAnchor* anchor = NULL;
                ArAnchor* anchor = NULL;
                auto anchor_status = ArHitResult_acquireNewAnchor(m_impl->XrContext->Session, ar_hit, &anchor);


                if (anchor_status != AR_SUCCESS) return;
                if (anchor == NULL) return;
                //LOGD("AEA anchor save e_anchor not null\n");
                auto anchor_ptr = std::make_shared<ArAnchor*>(anchor);
                m_impl->XrContext->EarthAnchors[anchor_name] = std::move(anchor_ptr);
                *out_placed = true;

                ArHitResult_destroy(ar_hit);
                ArTrackable_release(ar_trackable);
                break;
            }
            ArHitResult_destroy(ar_hit);
            ArTrackable_release(ar_trackable);
        }
        ArHitResultList_destroy(hit_result_list);
    }

    void System::hitTestEarthAnchor(std::string anchor_name, float in_tap_x, float in_tap_y, bool *out_placed, float *out_quaternion_4, double *out_altitude, double *out_latitude, double *out_longitude) {
        if (m_impl->XrContext->Earth == NULL) return;
        //LOGD("ARG in hit test anchor\n");
        ArHitResultList* hit_result_list = NULL;
        ArHitResultList_create(m_impl->XrContext->Session, &hit_result_list);
        //LOGD("ARG after hit test result list, x=%.2f, y=%.2f", in_tap_x, in_tap_y);
        ArFrame_hitTest(m_impl->XrContext->Session, m_impl->XrContext->Frame, in_tap_x, in_tap_y, hit_result_list);
        //LOGD("ARG after ar frame hit test\n");
        int32_t hit_result_list_size = 0;
        ArHitResultList_getSize(m_impl->XrContext->Session, hit_result_list, &hit_result_list_size);
        //LOGD("ARG after hit result get size\n");
        // Returned hit-test results are sorted by increasing distance from the camera
        // or virtual ray's origin. The first hit result is often the most relevant
        // when responding to user input.
        ArHitResult* ar_hit = NULL;
        // ++i should be i++
        //LOGD("ARG before loop iter\n");
        for (int32_t i = 0; i < hit_result_list_size; i++) {
            //ArHitResult* ar_hit = NULL;
            ArHitResult_create(m_impl->XrContext->Session, &ar_hit);
            //LOGD("ARG after hit result create\n");
            ArHitResultList_getItem(m_impl->XrContext->Session, hit_result_list, i, ar_hit);
            //LOGD("ARG after hit get item\n");
            if (ar_hit == NULL) {
                //LOGE("No item was hit.");
                return;
            }

            ArTrackable* ar_trackable = NULL;
            ArHitResult_acquireTrackable(m_impl->XrContext->Session, ar_hit, &ar_trackable);
            ArTrackableType ar_trackable_type = AR_TRACKABLE_NOT_VALID;
            ArTrackable_getType(m_impl->XrContext->Session, ar_trackable, &ar_trackable_type);
            // Creates an anchor if a plane was hit.
            if (ar_trackable_type == AR_TRACKABLE_PLANE) {
                // Do something with this hit result. For example, create an anchor at
                // this point of interest.
                //ArAnchor* anchor = NULL;
                ArPose* out_pose = NULL;
                ArHitResult_getHitPose(
                        m_impl->XrContext->Session,
                        ar_hit,
                        out_pose
                );
                if (out_pose == NULL) return;
                ArGeospatialPose* out_geospatial_pose = NULL;
                ArStatus tracking_status = AR_ERROR_NOT_TRACKING;
                tracking_status = ArEarth_getGeospatialPose(
                        m_impl->XrContext->Session,
                        m_impl->XrContext->Earth,
                        out_pose,
                        out_geospatial_pose
                );
                if (tracking_status != AR_SUCCESS || out_geospatial_pose == NULL) return;
                ArGeospatialPose_getEastUpSouthQuaternion(m_impl->XrContext->Session, out_geospatial_pose, out_quaternion_4);

                ArGeospatialPose_getAltitude(m_impl->XrContext->Session, out_geospatial_pose, out_altitude);

                ArGeospatialPose_getLatitudeLongitude(m_impl->XrContext->Session, out_geospatial_pose, out_latitude, out_longitude);
                addEarthAnchor(anchor_name, out_quaternion_4, *out_latitude, *out_longitude, *out_altitude, out_placed);

                //ArHitResult_acquireNewAnchor(m_impl->XrContext->Session, ar_hit, &anchor);

                // TODO: Use this anchor in your AR experience.
                //m_impl->XrContext->EarthAnchors->emplace(anchor_name, earth_anchor);
                /*
                ArAnchor_release(anchor);
                ArHitResult_destroy(ar_hit);
                ArTrackable_release(ar_trackable);
                 */
                break;
            }
            ArHitResult_destroy(ar_hit);
            ArTrackable_release(ar_trackable);
        }
        ArHitResultList_destroy(hit_result_list);
    }

    void System::getTerrainAnchorPose(std::string anchor_name, float *out_matrix, bool *out_tracked) {
        std::shared_ptr<ArAnchor*> ar_anchor;
        auto jt = m_impl->XrContext->EarthAnchors.find(anchor_name);
        if (jt != m_impl->XrContext->EarthAnchors.end()) {
            ar_anchor = jt->second;

            //ArTerrainAnchorState terrain_anchor_state;
            //ArAnchor_getTerrainAnchorState(m_impl->XrContext->Session, *ar_anchor,
            //                               &terrain_anchor_state);
            //if (terrain_anchor_state != AR_TERRAIN_ANCHOR_STATE_SUCCESS) return;
            ArTrackingState tracking_state;
            ArAnchor_getTrackingState(m_impl->XrContext->Session, *ar_anchor, &tracking_state);
            if (tracking_state == AR_TRACKING_STATE_TRACKING) {
                ArPose *out_pose = NULL;
                ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
                ArAnchor_getPose(m_impl->XrContext->Session, *ar_anchor, out_pose);
                ArPose_getMatrix(m_impl->XrContext->Session, out_pose, out_matrix);
                *out_tracked = true;
            }
        } else {
            std::shared_ptr<ArResolveAnchorOnTerrainFuture*> ar_future;
            auto jt2 = m_impl->XrContext->Futures.find(anchor_name);
            if (jt2 != m_impl->XrContext->Futures.end()) {
                //LOGD("GTAP future!\n");
                ar_future = jt2->second;
                ArFutureState ar_future_state;
                //LOGD("GTAP future bef get state!\n");
                ArFuture_getState(m_impl->XrContext->Session, reinterpret_cast<ArFuture*>(*ar_future), &ar_future_state);
                //LOGD("GTAP future af get state!\n");
                if (ar_future_state == AR_FUTURE_STATE_DONE) {
                    ArAnchor* earth_anchor = NULL;
                    //LOGD("GTAP future bef acq result anchor!\n");
                    ArResolveAnchorOnTerrainFuture_acquireResultAnchor(m_impl->XrContext->Session,
                                                                       *ar_future, &earth_anchor);

                    //LOGD("GTAP future af acq result anchor!\n");
                    auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
                    m_impl->XrContext->EarthAnchors[anchor_name] = std::move(earth_anchor_ptr);
                    //LOGD("GTAP future af emplace!\n");
                    ArTrackingState tracking_state;
                    //LOGD("GTAP future bef get tracking state!\n");
                    ArAnchor_getTrackingState(m_impl->XrContext->Session, earth_anchor, &tracking_state);
                    //LOGD("GTAP future af get tracking state!\n");
                    if (tracking_state == AR_TRACKING_STATE_TRACKING) {
                        ArPose *out_pose = NULL;
                        ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
                        ArAnchor_getPose(m_impl->XrContext->Session, earth_anchor, out_pose);
                        ArPose_getMatrix(m_impl->XrContext->Session, out_pose, out_matrix);
                        *out_tracked = true;
                    }
                    ArFuture_release(reinterpret_cast<ArFuture*>(*ar_future));
                    m_impl->XrContext->Futures.erase(anchor_name);
                }
            } else {
                std::shared_ptr<ArResolveCloudAnchorFuture*> ar_future;
                auto jt2 = m_impl->XrContext->CloudAnchorResolvingFutures.find(anchor_name);
                if (jt2 != m_impl->XrContext->CloudAnchorResolvingFutures.end()) {
                    //LOGD("Cloud resolving future!\n");
                    ar_future = jt2->second;
                    ArFutureState ar_future_state;
                    //LOGD("GTAP future bef get state!\n");
                    ArFuture_getState(m_impl->XrContext->Session, reinterpret_cast<ArFuture*>(*ar_future), &ar_future_state);
                    //LOGD("GTAP future af get state!\n");
                    //LOGD("GTAP future cloud state is %d\n", ar_future_state);
                    if (ar_future_state == AR_FUTURE_STATE_DONE) {
                        ArAnchor* earth_anchor = NULL;
                        //LOGD("GTAP future bef acq result anchor!\n");
                        ArCloudAnchorState cloud_anchor_state;
                        ArResolveCloudAnchorFuture_getResultCloudAnchorState(m_impl->XrContext->Session,
                                                                           *ar_future, &cloud_anchor_state);
                        //LOGD("GTAP future bef cloud anchor state!\n");
                        //LOGD("GTAP cloud anchor state: %d\n", cloud_anchor_state);
                        if (cloud_anchor_state == AR_CLOUD_ANCHOR_STATE_SUCCESS) {
                            ArResolveCloudAnchorFuture_acquireResultAnchor(m_impl->XrContext->Session,
                                                                           *ar_future, &earth_anchor);
                            //LOGD("GTAP future af acq result anchor!\n");
                            auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
                            m_impl->XrContext->EarthAnchors[anchor_name] = std::move(earth_anchor_ptr);
                            //LOGD("GTAP future af emplace!\n");
                            ArTrackingState tracking_state;
                            //LOGD("GTAP future bef get tracking state!\n");
                            ArAnchor_getTrackingState(m_impl->XrContext->Session, earth_anchor, &tracking_state);
                            //LOGD("GTAP future af get tracking state!\n");
                            if (tracking_state == AR_TRACKING_STATE_TRACKING) {
                                //LOGD("GTAP future af get tracking state, State is tracking!\n");
                                ArPose *out_pose = NULL;
                                ArPose_create(m_impl->XrContext->Session, NULL, &out_pose);
                                ArAnchor_getPose(m_impl->XrContext->Session, earth_anchor, out_pose);
                                ArPose_getMatrix(m_impl->XrContext->Session, out_pose, out_matrix);
                            }
                            ArFuture_release(reinterpret_cast<ArFuture*>(*ar_future));
                            m_impl->XrContext->CloudAnchorResolvingFutures.erase(anchor_name);
                        }
                    }
                }
            }
        }
    }


    void System::addTerrainAnchor(std::string anchor_name, float *in_quaternion_4, double in_latitude, double in_longitude, double in_altitude, bool *out_placed) {
        if (m_impl->XrContext->Earth != NULL) {
            ArTrackingState earth_tracking_state = AR_TRACKING_STATE_STOPPED;
            ArTrackable_getTrackingState(m_impl->XrContext->Session, (ArTrackable*)m_impl->XrContext->Earth,
                                         &earth_tracking_state);
            if (earth_tracking_state == AR_TRACKING_STATE_TRACKING) {
                //ArAnchor* earth_anchor = NULL;
                ArResolveAnchorOnTerrainFuture* ar_future = NULL;

                //LOGD("Add Terrain Anchor in anchor name! %s\n", anchor_name.c_str());
                auto anchor_status = ArEarth_resolveAnchorOnTerrainAsync(
                        m_impl->XrContext->Session, m_impl->XrContext->Earth,
                        /* Locational values */
                        in_latitude, in_longitude, in_altitude, in_quaternion_4,
                        NULL, [](void * context, ArAnchor *anchor, ArTerrainAnchorState terrain_anchor_state) {
                            (void) context;
                            (void) anchor;
                            (void) terrain_anchor_state;
                            //LOGD("Add Terrain Anchor in cb!\n");
                        }, &ar_future);

                if (ar_future == NULL) return;
                auto future_ptr = std::make_shared<ArResolveAnchorOnTerrainFuture*>(ar_future);
                //auto future_ptr = std::make_shared<ArFuture*>(ar_future);
                m_impl->XrContext->Futures[anchor_name] = std::move(future_ptr);
                // This anchor can't be used immediately; check its ArTrackingState
                // and ArTerrainAnchorState before rendering content on this anchor.
                if (anchor_status != AR_SUCCESS) return;
                //LOGD("AEA anchor pre-save\n");
                //if (earth_anchor == NULL) return;
                //LOGD("AEA anchor save e_anchor not null\n");
                //auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
                //m_impl->XrContext->EarthAnchors.emplace(anchor_name, std::move(earth_anchor_ptr));
                *out_placed = true;
            }
        }
    }

    void System::addEarthAnchor(std::string anchor_name, float *in_quaternion_4, double in_latitude, double in_longitude, double in_altitude, bool *out_placed) {
        if (m_impl->XrContext->Earth != NULL) {
            //LOGD("AEA in func\n");
            ArTrackingState earth_tracking_state = AR_TRACKING_STATE_STOPPED;
            ArTrackable_getTrackingState(m_impl->XrContext->Session, (ArTrackable*)m_impl->XrContext->Earth,
                                         &earth_tracking_state);

            //LOGD("AEA tracking state test\n");
            if (earth_tracking_state == AR_TRACKING_STATE_TRACKING) {
                ArAnchor* earth_anchor = NULL;
                auto anchor_status = ArEarth_acquireNewAnchor(m_impl->XrContext->Session, m_impl->XrContext->Earth,
                        /* Locational values */
                                                              in_latitude, in_longitude, in_altitude,
                                                              in_quaternion_4, &earth_anchor);
                //LOGD("AEA anchor create\n");
                if (anchor_status != AR_SUCCESS) return;
                //LOGD("AEA anchor pre-save\n");
                if (earth_anchor == NULL) return;
                //LOGD("AEA anchor save e_anchor not null\n");
                auto earth_anchor_ptr = std::make_shared<ArAnchor*>(earth_anchor);
                m_impl->XrContext->EarthAnchors[anchor_name] = std::move(earth_anchor_ptr);
                *out_placed = true;
            }
        }
    }


    void System::getEarthQuaternionLatitudeLongitude(float *out_quaternion_4, double *out_latitude, double *out_longitude, double *out_altitude, double *out_horizontal_accuracy, double *out_orientation_yaw_accuracy_degrees, double *out_vertical_accuracy, bool *out_success) {
        //LOGD("In getEarthQuaternionLatitudeLongitude\n");
        if (m_impl->XrContext->Earth != NULL) {
            //LOGD("In getEarthQuaternionLatitudeLongitude Earth OK\n");
            ArTrackingState earth_tracking_state = AR_TRACKING_STATE_STOPPED;
            ArTrackable_getTrackingState(m_impl->XrContext->Session, (ArTrackable*)m_impl->XrContext->Earth,
                                         &earth_tracking_state);
            if (earth_tracking_state == AR_TRACKING_STATE_TRACKING) {
                //LOGD("In getEarthQuaternionLatitudeLongitude Tracking OK\n");
                ArGeospatialPose* camera_geospatial_pose = NULL;
                ArGeospatialPose_create(m_impl->XrContext->Session, &camera_geospatial_pose);
                ArEarth_getCameraGeospatialPose(m_impl->XrContext->Session, m_impl->XrContext->Earth,
                                                camera_geospatial_pose);
                ArGeospatialPose_getEastUpSouthQuaternion(m_impl->XrContext->Session, camera_geospatial_pose, out_quaternion_4);
                ArGeospatialPose_getLatitudeLongitude(m_impl->XrContext->Session, camera_geospatial_pose, out_latitude, out_longitude);

                ArGeospatialPose_getAltitude(m_impl->XrContext->Session, camera_geospatial_pose, out_altitude);
                // camera_geospatial_pose contains geodetic location, rotation, and
                // confidences values.
                ArGeospatialPose_getHorizontalAccuracy(m_impl->XrContext->Session, camera_geospatial_pose, out_horizontal_accuracy);
                ArGeospatialPose_getOrientationYawAccuracy(m_impl->XrContext->Session, camera_geospatial_pose, out_orientation_yaw_accuracy_degrees);
                ArGeospatialPose_getVerticalAccuracy(m_impl->XrContext->Session, camera_geospatial_pose, out_vertical_accuracy);

                *out_success = true;

                ArGeospatialPose_destroy(camera_geospatial_pose);
            }
        }
    }



    arcana::task<std::shared_ptr<System::Session>, std::exception_ptr> System::Session::CreateAsync(System& system, void* graphicsDevice, void* commandQueue, std::function<void*()> windowProvider)
    {
        // First perform the ARCore installation check, request install if not yet installed.
        return CheckAndInstallARCoreAsync().then(arcana::inline_scheduler, arcana::cancellation::none(), []()
        {
            // Next check for camera permissions, and request if not already granted.
            return android::Permissions::CheckCameraPermissionAsync();
        }).then(arcana::inline_scheduler, arcana::cancellation::none(), [&system, graphicsDevice, commandQueue, windowProvider{ std::move(windowProvider) }]()
        {
            // Finally if the previous two tasks succeed, start the AR session.
            return std::make_shared<System::Session>(system, graphicsDevice, commandQueue, windowProvider);
        });
    }

    System::Session::Session(System& system, void* graphicsDevice, void*, std::function<void*()> windowProvider)
        : m_impl{ std::make_unique<System::Session::Impl>(*system.m_impl, graphicsDevice, std::move(windowProvider)) }
    {}

    System::Session::~Session()
    {
    }

    std::unique_ptr<System::Session::Frame> System::Session::GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession, std::function<arcana::task<void, std::exception_ptr>(void*)> deletedTextureAsyncCallback)
    {
        return m_impl->GetNextFrame(shouldEndSession, shouldRestartSession, deletedTextureAsyncCallback);
    }

    void System::Session::RequestEndSession()
    {
        m_impl->RequestEndSession();
    }

    void System::Session::SetDepthsNearFar(float depthNear, float depthFar)
    {
        m_impl->DepthNearZ = depthNear;
        m_impl->DepthFarZ = depthFar;
    }

    void System::Session::SetPlaneDetectionEnabled(bool enabled) const
    {
        m_impl->PlaneDetectionEnabled = enabled;
    }

    bool System::Session::TrySetFeaturePointCloudEnabled(bool enabled) const
    {
        // Point cloud system not yet supported.
        m_impl->FeaturePointCloudEnabled = enabled;
        return enabled;
    }

    bool System::Session::TrySetPreferredPlaneDetectorOptions(const xr::GeometryDetectorOptions& /*options*/)
    {
        // TODO
        return false;
    }

    bool System::Session::TrySetMeshDetectorEnabled(const bool /*enabled*/)
    {
        // TODO
        return false;
    }

    bool System::Session::TrySetPreferredMeshDetectorOptions(const xr::GeometryDetectorOptions& /*options*/)
    {
        // TODO
        return false;
    }

    std::vector<ImageTrackingScore>* System::Session::GetImageTrackingScores() const
    {
        return m_impl->GetImageTrackingScores();
    }

    void System::Session::CreateAugmentedImageDatabase(const std::vector<System::Session::ImageTrackingRequest>& bitmaps) const
    {
        return m_impl->CreateAugmentedImageDatabase(bitmaps);
    }
}
