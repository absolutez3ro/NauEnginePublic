#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/KeyboardDevice.h"

#include <Windows.h>
#include "nau/input.h"

namespace
{
    bool TryMapKey(const unsigned char keycode, nau::input::Key& outKey)
    {
        using nau::input::Key;

        if (keycode >= 'A' && keycode <= 'Z')
        {
            outKey = static_cast<Key>(static_cast<int>(Key::A) + (keycode - 'A'));
            return true;
        }

        if (keycode >= '0' && keycode <= '9')
        {
            outKey = static_cast<Key>(static_cast<int>(Key::N0) + (keycode - '0'));
            return true;
        }

        if (keycode == VK_SPACE)
        {
            outKey = Key::Space;
            return true;
        }

        return false;
    }
}

void KeyboardDevice::SetDeviceId(const int id)
{
    deviceId = id;
}

bool KeyboardDevice::KeyIsPressed(const unsigned char keycode) const
{
    if (keycode == VK_SHIFT)
    {
        return nau::input::isKeyboardButtonHold(deviceId, nau::input::Key::ShiftL) ||
            nau::input::isKeyboardButtonHold(deviceId, nau::input::Key::ShiftR);
    }

    nau::input::Key key;
    if (!TryMapKey(keycode, key))
    {
        return false;
    }

    return nau::input::isKeyboardButtonHold(deviceId, key);
}
