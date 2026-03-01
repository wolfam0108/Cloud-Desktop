#pragma once

// [cloud-mouse] D-Bus сервис для экспонирования libinput устройств.
// Bus name: org.gamescope.Input
// Позволяет KCM Cloud Mouse читать/менять настройки мыши.

#include "waitable.h"

#include <systemd/sd-bus.h>
#include <libinput.h>

#include <map>
#include <string>
#include <vector>
#include <mutex>

namespace gamescope
{
    class CInputDeviceDBus final : public IWaitable
    {
    public:
        CInputDeviceDBus();
        ~CInputDeviceDBus();

        bool Init();
        void Shutdown();

        // Вызывается из CLibInputHandler при DEVICE_ADDED/REMOVED
        void OnDeviceAdded( libinput_device *pDevice );
        void OnDeviceRemoved( libinput_device *pDevice );

        // IWaitable
        virtual int GetFD() override;
        virtual void OnPollIn() override;

        // Используется property handlers
        libinput_device *FindDevice( const char *pszSysName );
        std::vector<std::string> GetDeviceSysNames();

        // scrollFactor хранится in-memory (libinput не имеет этого)
        double GetScrollFactor( const char *pszSysName );
        void SetScrollFactor( const char *pszSysName, double flFactor );

    private:
        bool RegisterDevice( libinput_device *pDevice );
        void UnregisterDevice( const std::string &sSysName );
        void SaveDeviceConfig( const char *pszSysName, libinput_device *pDev );
        void LoadDeviceConfig( const char *pszSysName, libinput_device *pDev );

        sd_bus *m_pBus = nullptr;
        sd_bus_slot *m_pManagerSlot = nullptr;

        struct DeviceInfo
        {
            libinput_device *pDevice;
            sd_bus_slot *pSlot;
            double flScrollFactor;
        };

        std::map<std::string, DeviceInfo> m_devices;
        std::mutex m_mutex;

        // sd-bus vtable определения (static)
        static const sd_bus_vtable s_DeviceVTable[];
        static const sd_bus_vtable s_ManagerVTable[];

        // Property handlers
        static int DevicePropertyGet( sd_bus *bus, const char *path, const char *interface,
            const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *error );
        static int DevicePropertySet( sd_bus *bus, const char *path, const char *interface,
            const char *property, sd_bus_message *value, void *userdata, sd_bus_error *error );

        // Manager property handler
        static int ManagerPropertyGet( sd_bus *bus, const char *path, const char *interface,
            const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *error );
    };
}
