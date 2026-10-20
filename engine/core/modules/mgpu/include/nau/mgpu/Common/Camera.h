#pragma once
#include "SimpleMath.h"
#include "nau/mgpu/Utils/d3dUtil.h"

namespace nau::scene
{
    struct ICameraProperties;
}

using namespace nau::PEPEngine;
using namespace Utils;

using namespace DirectX::SimpleMath;

// Adapter that replaces Camera from A (a Component of GameObject, which B does not have).
// View/projection are built left-handed as in A (LookAtLH / PerspectiveFovLH).
// When a Nau camera is bound with SetSource, the world transform, fov (degrees) and near/far planes are taken
// from it. Local axes of a Nau camera: -Z is forward, +Y is up.
class Camera
{
    Matrix view = Matrix::Identity;
    Matrix projection = Matrix::Identity;

    float fov = 60;
    float aspectRatio = 0;
    float nearZ = 0.1f;
    float farZ = 10000;

    Vector3 focusPosition = Vector3::Zero;

    const nau::scene::ICameraProperties* source = nullptr;

    int NumFramesDirty = globalCountFrameResources;

    void CreateProjection();

public:
    const Vector3& GetFocusPosition() const;

    Camera(float aspect);

    void SetSource(const nau::scene::ICameraProperties* cameraProperties);

    // World transform of the camera when no Nau camera is bound (A: Transform of the camera GameObject).
    // Same axes as for a bound Nau camera.
    void SetWorldMatrix(const Matrix& world);

    void Update();

    void SetAspectRatio(float aspect);

    void SetFov(float fov);

    float GetFov() const;

    const Matrix& GetViewMatrix() const;

    const Matrix& GetProjectionMatrix() const;
};
