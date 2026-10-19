#pragma once

struct MousePoint
{
    int x;
    int y;
};

// Thin adapter over the Nau input module (nau/input.h) that replaces Mousepad from A.
// Only the polling part of the A interface is provided: the event queue of A
// (ReadEvent/EventBufferIsEmpty/On*) has no equivalent in Nau and is not carried over.
class Mousepad
{
public:
    Mousepad() = default;

    bool IsLeftDown() const;
    bool IsMiddleDown() const;
    bool IsRightDown() const;

    int GetPosX() const;
    int GetPosY() const;
    MousePoint GetPos() const;

    void SetDeviceId(int id);

private:
    int deviceId = 0;
};
