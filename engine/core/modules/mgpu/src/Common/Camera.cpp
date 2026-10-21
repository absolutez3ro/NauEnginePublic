#include "nau/mgpu/Common/pch.h"

// d3dUtil.h must precede the Nau headers: nau/math defines a `PI` macro that breaks Math::PI.
#include "nau/mgpu/Utils/d3dUtil.h"

// Must precede Camera.h: it has a global `using namespace DirectX::SimpleMath`, which makes Vector2/3/4
// ambiguous with the Vectormath types used by nau/math.
#include "nau/scene/camera/camera.h"

#include "nau/mgpu/Common/Camera.h"

namespace
{
    // Nau Matrix4 is column-major with column vectors (translation in column 3);
    // SimpleMath::Matrix is row-major with row vectors, so column i becomes row i.
    Matrix ToSimpleMath(const nau::math::mat4& m)
    {
        Matrix result;
        for (int col = 0; col < 4; ++col)
        {
            const auto c = m.getCol(col);
            result.m[col][0] = static_cast<float>(c.getX());
            result.m[col][1] = static_cast<float>(c.getY());
            result.m[col][2] = static_cast<float>(c.getZ());
            result.m[col][3] = static_cast<float>(c.getW());
        }
        return result;
    }
}

void Camera::Update()
{
    if (source)
    {
        SetWorldMatrix(ToSimpleMath(source->getWorldTransform().getMatrix()));

        if (fov != source->getFov() || nearZ != source->getClipNearPlane() || farZ != source->getClipFarPlane())
        {
            fov = source->getFov();
            nearZ = source->getClipNearPlane();
            farZ = source->getClipFarPlane();
            NumFramesDirty = globalCountFrameResources;
        }
    }

    if (NumFramesDirty > 0)
    {
        CreateProjection();
        NumFramesDirty--;
    }
}

void Camera::SetWorldMatrix(const Matrix& world)
{
    const Vector3 position(world.m[3][0], world.m[3][1], world.m[3][2]);
    Vector3 forward = -Vector3(world.m[2][0], world.m[2][1], world.m[2][2]);
    Vector3 up(world.m[1][0], world.m[1][1], world.m[1][2]);
    forward.Normalize();
    up.Normalize();

    focusPosition = forward + position;

    view = XMMatrixLookAtLH(position, focusPosition, up);
}

void Camera::CreateProjection()
{
    const float fovRadians = DirectX::XMConvertToRadians(fov);
    projection = DirectX::XMMatrixPerspectiveFovLH(fovRadians, aspectRatio, nearZ, farZ);
}

const Vector3& Camera::GetFocusPosition() const
{
    return focusPosition;
}

Camera::Camera(const float aspect) : aspectRatio(aspect)
{
}

void Camera::SetSource(const nau::scene::ICameraProperties* cameraProperties)
{
    source = cameraProperties;
    NumFramesDirty = globalCountFrameResources;
}

void Camera::SetAspectRatio(const float aspect)
{
    aspectRatio = aspect;
    NumFramesDirty = globalCountFrameResources;
}

void Camera::SetFov(const float fov)
{
    this->fov = fov;
    NumFramesDirty = globalCountFrameResources;
}

float Camera::GetFov() const
{
    return fov;
}

const Matrix& Camera::GetViewMatrix() const
{
    return this->view;
}

const Matrix& Camera::GetProjectionMatrix() const
{
    return this->projection;
}
