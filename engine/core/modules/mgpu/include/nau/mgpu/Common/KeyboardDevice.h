#pragma once

// Thin adapter over the Nau input module (nau/input.h) that replaces KeyboardDevice from A.
// Only the polling part of the A interface is provided: the event queues of A
// (ReadKey/ReadChar/OnKeyPressed/...) have no equivalent in Nau and are not carried over.
class KeyboardDevice
{
public:
    KeyboardDevice() = default;

    // Virtual-key code (VK_*, 'A'..'Z', '0'..'9') -> is the key currently held down.
    // Codes that have no Nau counterpart return false.
    bool KeyIsPressed(unsigned char keycode) const;

    void SetDeviceId(int id);

private:
    int deviceId = 0;
};
