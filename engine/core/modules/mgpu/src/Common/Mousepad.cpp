#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/Mousepad.h"

#include "nau/input.h"

void Mousepad::SetDeviceId(const int id)
{
    deviceId = id;
}

bool Mousepad::IsLeftDown() const
{
    return nau::input::isMouseButtonHold(deviceId, nau::input::MouseKey::ButtonLeft);
}

bool Mousepad::IsMiddleDown() const
{
    return nau::input::isMouseButtonHold(deviceId, nau::input::MouseKey::ButtonMiddle);
}

bool Mousepad::IsRightDown() const
{
    return nau::input::isMouseButtonHold(deviceId, nau::input::MouseKey::ButtonRight);
}

int Mousepad::GetPosX() const
{
    return static_cast<int>(nau::input::getMouseAxisValue(deviceId, nau::input::MouseKey::AxisX));
}

int Mousepad::GetPosY() const
{
    return static_cast<int>(nau::input::getMouseAxisValue(deviceId, nau::input::MouseKey::AxisY));
}

MousePoint Mousepad::GetPos() const
{
    return {GetPosX(), GetPosY()};
}
