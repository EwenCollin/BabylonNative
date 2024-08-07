#pragma once

#include <memory>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <arcana/threading/task.h>

namespace xr
{
    enum class TextureFormat
    {
        RGBA8_SRGB,
        BGRA8_SRGB,
        D24S8,
        D16
    };

    enum class SessionType
    {
        IMMERSIVE_VR,
        IMMERSIVE_AR,
        INLINE,
        INVALID
    };

    enum class PolygonFormat
    {
        XZ,
        XYZ
    };

    enum class HitTestTrackableType {
        NONE = 0,
        POINT = 1 << 0,
        PLANE = 1 << 1,
        MESH = 1 << 2,
    };

    enum class ImageTrackingScore
    {
        UNTRACKABLE,
        TRACKABLE,
    };

    enum class ImageTrackingState
    {
        UNTRACKED,
        TRACKED,
        EMULATED,
    };

    constexpr enum HitTestTrackableType operator |(const enum HitTestTrackableType selfValue, const enum HitTestTrackableType inValue)
    {
        return static_cast<const enum HitTestTrackableType>(std::underlying_type_t<HitTestTrackableType>(selfValue) | std::underlying_type_t<HitTestTrackableType>(inValue));
    }

    constexpr enum HitTestTrackableType operator &(const enum HitTestTrackableType selfValue, const enum HitTestTrackableType inValue)
    {
        return static_cast<const enum HitTestTrackableType>(std::underlying_type_t<HitTestTrackableType>(selfValue) & std::underlying_type_t<HitTestTrackableType>(inValue));
    }

    constexpr enum HitTestTrackableType& operator |=(enum HitTestTrackableType& selfValue, const enum HitTestTrackableType inValue)
    {
        selfValue = selfValue | inValue;
        return selfValue;
    }

    struct Size
    {
        size_t Width{};
        size_t Height{};
        size_t Depth{};
    };

    struct Vector3f
    {
        float X{0.f};
        float Y{0.f};
        float Z{0.f};
    };

    struct Vector4f
    {
        float X{0.f};
        float Y{0.f};
        float Z{0.f};
        float W{1.f};
    };

    struct Pose
    {
        Vector3f Position;
        Vector4f Orientation;
    };

    struct Space
    {
        Pose Pose;
    };

    using NativeTrackablePtr = void*;
    struct HitResult
    {
        Pose Pose{};
        NativeTrackablePtr NativeTrackable{};
    };

    struct Ray
    {
        Vector3f Origin;
        Vector3f Direction;
    };

    using NativeAnchorPtr = void*;
    struct Anchor
    {
        Space Space{};
        NativeAnchorPtr NativeAnchor{};
        bool IsValid{true};
    };

    struct FeaturePoint
    {
        using Identifier = size_t;

        float X{};
        float Y{};
        float Z{};
        float ConfidenceValue{};
        Identifier ID{};
    };

    struct FieldOfView
    {
        float AngleLeft;
        float AngleRight;
        float AngleUp;
        float AngleDown;
    };

    enum class DetectionBoundaryType
    {
        Box,
        Frustum,
        Sphere
    };

    struct Frustum
    {
        Pose Pose{};
        FieldOfView FOV{};
        float FarDistance;
    };

    struct DetectionBoundary
    {
        DetectionBoundaryType Type{ DetectionBoundaryType::Sphere };
        std::variant<float, Frustum, Vector3f> Data{ 5.f };
    };

    struct GeometryDetectorOptions
    {
        xr::DetectionBoundary DetectionBoundary{};
        double UpdateInterval{10};
    };

    enum class SceneObjectType
    {
        Unknown,
        Background,
        Wall,
        Floor,
        Ceiling,
        Platform,
        Inferred,
        Undefined
    };

    const std::map<xr::SceneObjectType, std::string> SceneObjectTypeNames
    {
        {xr::SceneObjectType::Unknown, "unknown" },
        {xr::SceneObjectType::Background, "background" },
        {xr::SceneObjectType::Ceiling, "ceiling" },
        {xr::SceneObjectType::Floor, "floor" },
        {xr::SceneObjectType::Platform, "platform" },
        {xr::SceneObjectType::Wall, "wall" },
        {xr::SceneObjectType::Inferred, "inferred" }
    };

    class System
    {
    public:
        static constexpr float DEFAULT_DEPTH_NEAR_Z{ 0.5f };
        static constexpr float DEFAULT_DEPTH_FAR_Z{ 1000.f };
        static constexpr uint32_t DEFAULT_CONTROLLER_BUTTONS_COUNT{ 4 };
        static constexpr uint32_t DEFAULT_CONTROLLER_AXES_COUNT{ 4 };

        class Session
        {
            friend class System;
            struct Impl;

        public:
            struct ImageTrackingRequest
            {
                const uint8_t* data{nullptr};
                uint32_t width{0};
                uint32_t height{0};
                uint32_t depth{0};
                uint32_t stride{0};
                float measuredWidthInMeters{0.0};
            };

            class Frame
            {
            public:
                struct JointSpace : Space
                {
                    float PoseRadius{};
                    bool PoseTracked{ false };
                };

                struct GamePad
                {
                    struct Button
                    {
                        bool Pressed{ false };
                        bool Touched{ false };
                        float Value{0};
                    };

                    std::vector<float> Axes{};
                    std::vector<Button> Buttons{};
                };

                struct View
                {
                    Space Space{};
                    std::array<float, 16> ProjectionMatrix{};

                    TextureFormat ColorTextureFormat{};
                    void* ColorTexturePointer{};
                    Size ColorTextureSize;

                    TextureFormat DepthTextureFormat{};
                    void* DepthTexturePointer{};
                    Size DepthTextureSize;

                    float DepthNearZ{};
                    float DepthFarZ{};

                    bool IsFirstPersonObserver{ false };
                    bool RequiresAppClear{ false };
                };

                struct InputSource
                {
                    using Identifier = size_t;

                    enum class HandednessEnum
                    {
                        Left = 0,
                        Right = 1
                    };

                    const Identifier ID{ NEXT_ID++ };
                    bool TrackedThisFrame{};
                    bool JointsTrackedThisFrame{};
                    bool GamepadTrackedThisFrame{};
                    bool HandTrackedThisFrame{};
                    std::string InteractionProfileName{""};
                    GamePad GamepadObject{};
                    Space GripSpace{};
                    Space AimSpace{};
                    HandednessEnum Handedness{};
                    std::vector<JointSpace> HandJoints{};

                private:
                    static inline Identifier NEXT_ID{ 0 };
                };

                struct SceneObject
                {
                    using Identifier = int32_t;
                    const static Identifier INVALID_ID = -1;
                    Identifier ID{ NEXT_ID++ };
                    SceneObjectType Type{ SceneObjectType::Undefined };

                private:
                    static inline Identifier NEXT_ID{ 0 };
                };

                struct Plane
                {
                    using Identifier = size_t;
                    const Identifier ID{ NEXT_ID++ };
                    Pose Center{};
                    std::vector<float> Polygon{};
                    size_t PolygonSize{0};
                    PolygonFormat PolygonFormat{};
                    SceneObject::Identifier ParentSceneObjectID{ SceneObject::INVALID_ID };

                private:
                    static inline Identifier NEXT_ID{ 0 };
                };

                struct Mesh
                {
                    using IndexType = uint32_t;
                    using Identifier = size_t;
                    const Identifier ID{ NEXT_ID++ };
                    std::vector<xr::Vector3f> Positions{};
                    std::vector<IndexType> Indices{};
                    bool HasNormals{ false };
                    std::vector<xr::Vector3f> Normals;
                    SceneObject::Identifier ParentSceneObjectID{ SceneObject::INVALID_ID };

                private:
                    static inline Identifier NEXT_ID{ 0 };
                };
                
                struct ImageTrackingResult
                {
                    using Identifier = size_t;
                    const Identifier ID{ NEXT_ID++ };
                    Space ImageSpace{};
                    uint32_t Index{0};
                    ImageTrackingState TrackingState{ImageTrackingState::UNTRACKED};
                    float MeasuredWidthInMeters{0};

                private:
                    static inline Identifier NEXT_ID{ 0 };
                };
                
                std::vector<View>& Views;
                std::vector<InputSource>& InputSources;
                std::vector<FeaturePoint>& FeaturePointCloud;

                std::optional<Space>& EyeTrackerSpace;

                std::vector<SceneObject::Identifier> UpdatedSceneObjects;
                std::vector<SceneObject::Identifier> RemovedSceneObjects;
                std::vector<Plane::Identifier> UpdatedPlanes;
                std::vector<Plane::Identifier> RemovedPlanes;
                std::vector<Mesh::Identifier> UpdatedMeshes;
                std::vector<Mesh::Identifier> RemovedMeshes;
                std::vector<ImageTrackingResult::Identifier> UpdatedImageTrackingResults;

                bool IsTracking;

                Frame(System::Session::Impl&);
                ~Frame();

                void GetHitTestResults(std::vector<HitResult>&, Ray, HitTestTrackableType) const;
                Anchor CreateAnchor(Pose, NativeAnchorPtr) const;
                Anchor DeclareAnchor(NativeAnchorPtr) const;
                void UpdateAnchor(Anchor&) const;
                void DeleteAnchor(Anchor&) const;
                SceneObject& GetSceneObjectByID(SceneObject::Identifier) const;
                Plane& GetPlaneByID(Plane::Identifier) const;
                Mesh& GetMeshByID(Mesh::Identifier) const;
                ImageTrackingResult& GetImageTrackingResultByID(ImageTrackingResult::Identifier) const;

            private:
                struct Impl;
                std::unique_ptr<Impl> m_impl{};
            };

            static arcana::task<std::shared_ptr<Session>, std::exception_ptr> CreateAsync(System& system, void* graphicsDevice, void* commandQueue, std::function<void*()> windowProvider);
            ~Session();

            // Do not use, call CreateAsync instead. Kept public to keep compatibility with make_shared.
            // Move to private when changing to unique_ptr.
            Session(System& system, void* graphicsDevice, void* commandQueue, std::function<void*()> windowProvider);

            std::unique_ptr<Frame> GetNextFrame(bool& shouldEndSession, bool& shouldRestartSession, std::function<arcana::task<void, std::exception_ptr>(void*)> deletedTextureAsyncCallback = [](void*){ return arcana::task_from_result<std::exception_ptr>(); });
            void RequestEndSession();
            void SetDepthsNearFar(float depthNear, float depthFar);
            void SetPlaneDetectionEnabled(bool enabled) const;
            bool TrySetFeaturePointCloudEnabled(bool enabled) const;

            bool TrySetPreferredPlaneDetectorOptions(const GeometryDetectorOptions& options);
            bool TrySetMeshDetectorEnabled(const bool enabled);
            bool TrySetPreferredMeshDetectorOptions(const GeometryDetectorOptions& options);

            std::vector<ImageTrackingScore>* GetImageTrackingScores() const;
            void CreateAugmentedImageDatabase(const std::vector<ImageTrackingRequest>&) const;
        private:
            std::unique_ptr<Impl> m_impl{};
        };

        System(const char* = "OpenXR Experience");
        ~System();

        bool IsInitialized() const;
        bool TryInitialize();
        static arcana::task<bool, std::exception_ptr> IsSessionSupportedAsync(SessionType);

        uintptr_t GetNativeXrContext();
        std::string GetNativeXrContextType();

        //add get earthQuaternion + lat,lon
        void addLocalEarthAnchor(std::string anchor_name, float *in_quaternion4_translation3, bool *out_placed, float *out_quaternion_4, double *out_altitude, double *out_latitude, double *out_longitude);
        void removeEarthAnchor(std::string anchor_name);
        void getEarthAnchorPose(std::string anchor_name, float *out_matrix, float *out_cam_matrix);
        void hitTestEarthAnchor(std::string anchor_name, float in_tap_x, float in_tap_y, bool *out_placed, float *out_quaternion_4, double *out_altitude, double *out_latitude, double *out_longitude);
        void addEarthAnchor(std::string anchor_name, float *in_quaternion_4, double in_latitude, double in_longitude, double in_altitude, bool *out_placed);
        void getEarthQuaternionLatitudeLongitude(float *out_quaternion_4, double *out_latitude, double *out_longitude, double *out_altitude, double *out_horizontal_accuracy, double *out_orientation_yaw_accuracy_degrees, double *out_vertical_accuracy, bool *out_success);
        void addTerrainAnchor(std::string anchor_name, float *in_quaternion_4, double in_latitude, double in_longitude, double in_altitude, bool *out_placed);
        void getTerrainAnchorPose(std::string anchor_name, float *out_matrix, bool *out_tracked);
        void hitTestAnchor(std::string anchor_name, float in_tap_x, float in_tap_y, bool *out_placed);
        void estimateFeatureMapQualityForHosting(std::string anchor_name, bool *is_good);
        void hostCloudAnchor(std::string anchor_name, int in_ttl_days, bool *out_error);
        void resolveCloudAnchor(std::string anchor_name, std::string cloud_anchor_id, bool *out_error);
        void getCloudAnchorHostStatus(std::string anchor_name, std::string *out_cloud_anchor_id, bool *out_hosted, bool *out_error);
        void getEarthAnchorGeospatialPose(std::string anchor_name, float *out_eus_quaternion4, double *out_latitude, double *out_longitude, double *out_altitude, bool *out_success, double *out_horizontal_accuracy, double *out_orientation_yaw_accuracy_degrees, double *out_vertical_accuracy);


    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl{};
    };
}
