#pragma once

#include "waitable.h"

struct libinput_interface;
struct udev;
struct libinput;

namespace gamescope
{
    class CInputDeviceDBus;

    class CLibInputHandler final : public IWaitable
    {
    public:
        CLibInputHandler();
        ~CLibInputHandler();

        bool Init();

        virtual int GetFD() override;
        virtual void OnPollIn() override;

        // [cloud-mouse] D-Bus integration
        void SetDBus( CInputDeviceDBus *pDBus ) { m_pDBus = pDBus; }
        libinput *GetLibInput() { return m_pLibInput; }

    private:
        udev *m_pUdev = nullptr;
        libinput *m_pLibInput = nullptr;

        double m_flScrollAccum[2]{};

        // [cloud-mouse] D-Bus сервис для input device properties
        CInputDeviceDBus *m_pDBus = nullptr;

        static const libinput_interface s_LibInputInterface;
    };
}