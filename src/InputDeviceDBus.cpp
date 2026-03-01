#include "InputDeviceDBus.h"
#include "log.hpp"

#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <libudev.h>
#include <linux/input-event-codes.h>

// Дефолтный scrollFactor — калиброван под Windows-подобное поведение
static constexpr double kDefaultScrollFactor = 8.0;

// [cloud-mouse] D-Bus сервис org.gamescope.Input
// Экспонирует libinput устройства для KCM Cloud Mouse.

namespace gamescope
{
    static LogScope log_dbus( "InputDBus" );

    // ========================================================================
    // sd-bus vtable: org.gamescope.InputDevice (per-device properties)
    // ========================================================================

    // Helper: извлечь sysName из D-Bus object path
    // "/org/gamescope/Input/Device/event0" → "event0"
    static const char *SysNameFromPath( const char *pszPath )
    {
        const char *pszPrefix = "/org/gamescope/Input/Device/";
        if ( strncmp( pszPath, pszPrefix, strlen( pszPrefix ) ) != 0 )
            return nullptr;
        return pszPath + strlen( pszPrefix );
    }

    int CInputDeviceDBus::DevicePropertyGet(
        sd_bus *bus, const char *path, const char *interface,
        const char *property, sd_bus_message *reply,
        void *userdata, sd_bus_error *error )
    {
        auto *pSelf = static_cast<CInputDeviceDBus*>( userdata );
        const char *pszSysName = SysNameFromPath( path );
        if ( !pszSysName )
            return sd_bus_error_setf( error, SD_BUS_ERROR_INVALID_ARGS, "Invalid path" );

        libinput_device *pDev = pSelf->FindDevice( pszSysName );
        if ( !pDev )
            return sd_bus_error_setf( error, SD_BUS_ERROR_UNKNOWN_OBJECT, "Device not found: %s", pszSysName );
        (void)pDev;

        // String properties
        if ( strcmp( property, "name" ) == 0 )
        {
            const char *pszName = libinput_device_get_name( pDev );
            return sd_bus_message_append( reply, "s", pszName ? pszName : "" );
        }
        if ( strcmp( property, "sysName" ) == 0 )
        {
            return sd_bus_message_append( reply, "s", pszSysName );
        }

        // Bool properties
        if ( strcmp( property, "pointer" ) == 0 )
            return sd_bus_message_append( reply, "b", (int)libinput_device_has_capability( pDev, LIBINPUT_DEVICE_CAP_POINTER ) );
        if ( strcmp( property, "keyboard" ) == 0 )
            return sd_bus_message_append( reply, "b", (int)libinput_device_has_capability( pDev, LIBINPUT_DEVICE_CAP_KEYBOARD ) );
        if ( strcmp( property, "touchpad" ) == 0 )
        {
            // touchpad = pointer + udev ID_INPUT_TOUCHPAD
            bool bPointer = libinput_device_has_capability( pDev, LIBINPUT_DEVICE_CAP_POINTER );
            bool bTouchpad = false;
            if ( bPointer )
            {
                struct udev_device *pUdev = libinput_device_get_udev_device( pDev );
                if ( pUdev )
                {
                    bTouchpad = udev_device_get_property_value( pUdev, "ID_INPUT_TOUCHPAD" ) != nullptr;
                    udev_device_unref( pUdev );
                }
            }
            return sd_bus_message_append( reply, "b", (int)bTouchpad );
        }
        if ( strcmp( property, "touch" ) == 0 )
            return sd_bus_message_append( reply, "b", (int)libinput_device_has_capability( pDev, LIBINPUT_DEVICE_CAP_TOUCH ) );

        // Pointer acceleration
        if ( strcmp( property, "supportsPointerAcceleration" ) == 0 )
            return sd_bus_message_append( reply, "b", (int)libinput_device_config_accel_is_available( pDev ) );
        if ( strcmp( property, "pointerAcceleration" ) == 0 )
            return sd_bus_message_append( reply, "d", libinput_device_config_accel_get_speed( pDev ) );
        if ( strcmp( property, "defaultPointerAcceleration" ) == 0 )
            return sd_bus_message_append( reply, "d", libinput_device_config_accel_get_default_speed( pDev ) );

        // Acceleration profiles
        if ( strcmp( property, "supportsPointerAccelerationProfileFlat" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_accel_get_profiles( pDev ) & LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT ) );
        if ( strcmp( property, "defaultPointerAccelerationProfileFlat" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_accel_get_default_profile( pDev ) == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT ) );
        if ( strcmp( property, "pointerAccelerationProfileFlat" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_accel_get_profile( pDev ) == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT ) );

        if ( strcmp( property, "supportsPointerAccelerationProfileAdaptive" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_accel_get_profiles( pDev ) & LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE ) );
        if ( strcmp( property, "defaultPointerAccelerationProfileAdaptive" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_accel_get_default_profile( pDev ) == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE ) );
        if ( strcmp( property, "pointerAccelerationProfileAdaptive" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_accel_get_profile( pDev ) == LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE ) );

        // Natural scroll
        if ( strcmp( property, "supportsNaturalScroll" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_scroll_has_natural_scroll( pDev ) );
        if ( strcmp( property, "naturalScrollEnabledByDefault" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_scroll_get_default_natural_scroll_enabled( pDev ) );
        if ( strcmp( property, "naturalScroll" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_scroll_get_natural_scroll_enabled( pDev ) );

        // Scroll factor (in-memory, no libinput API)
        if ( strcmp( property, "scrollFactor" ) == 0 )
            return sd_bus_message_append( reply, "d", pSelf->GetScrollFactor( pszSysName ) );

        // Left-handed
        if ( strcmp( property, "supportsLeftHanded" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_left_handed_is_available( pDev ) );
        if ( strcmp( property, "leftHandedEnabledByDefault" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_left_handed_get_default( pDev ) );
        if ( strcmp( property, "leftHanded" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_left_handed_get( pDev ) );

        // Middle emulation
        if ( strcmp( property, "supportsMiddleEmulation" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)libinput_device_config_middle_emulation_is_available( pDev ) );
        if ( strcmp( property, "middleEmulationEnabledByDefault" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_middle_emulation_get_default_enabled( pDev ) == LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED ) );
        if ( strcmp( property, "middleEmulation" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_middle_emulation_get_enabled( pDev ) == LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED ) );

        // Disable events
        if ( strcmp( property, "supportsDisableEvents" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_send_events_get_modes( pDev ) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED ) );
        if ( strcmp( property, "enabled" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( ( libinput_device_config_send_events_get_mode( pDev ) & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED ) == 0 ) );

        // Scroll on button down
        if ( strcmp( property, "supportsScrollOnButtonDown" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_scroll_get_methods( pDev ) & LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN ) );
        if ( strcmp( property, "scrollOnButtonDownEnabledByDefault" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_scroll_get_default_method( pDev ) == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN ) );
        if ( strcmp( property, "scrollOnButtonDown" ) == 0 )
            return sd_bus_message_append( reply, "b",
                (int)( libinput_device_config_scroll_get_method( pDev ) == LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN ) );

        // Supported buttons (bitmask as int)
        if ( strcmp( property, "supportedButtons" ) == 0 )
        {
            // Return mouse button bitmask
            int nButtons = 0;
            if ( libinput_device_pointer_has_button( pDev, BTN_LEFT ) ) nButtons |= 1;
            if ( libinput_device_pointer_has_button( pDev, BTN_RIGHT ) ) nButtons |= 2;
            if ( libinput_device_pointer_has_button( pDev, BTN_MIDDLE ) ) nButtons |= 4;
            return sd_bus_message_append( reply, "i", nButtons );
        }

        return sd_bus_error_setf( error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property: %s", property );
    }

    int CInputDeviceDBus::DevicePropertySet(
        sd_bus *bus, const char *path, const char *interface,
        const char *property, sd_bus_message *value,
        void *userdata, sd_bus_error *error )
    {
        auto *pSelf = static_cast<CInputDeviceDBus*>( userdata );
        const char *pszSysName = SysNameFromPath( path );
        if ( !pszSysName )
            return sd_bus_error_setf( error, SD_BUS_ERROR_INVALID_ARGS, "Invalid path" );

        libinput_device *pDev = pSelf->FindDevice( pszSysName );
        if ( !pDev )
            return sd_bus_error_setf( error, SD_BUS_ERROR_UNKNOWN_OBJECT, "Device not found: %s", pszSysName );

        if ( strcmp( property, "pointerAcceleration" ) == 0 )
        {
            double flAccel = 0.0;
            int r = sd_bus_message_read( value, "d", &flAccel );
            if ( r < 0 ) return r;
            auto status = libinput_device_config_accel_set_speed( pDev, flAccel );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: pointerAcceleration = %.3f", pszSysName, flAccel );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "pointerAccelerationProfileFlat" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto profile = bSet ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            auto status = libinput_device_config_accel_set_profile( pDev, profile );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: accel profile = %s", pszSysName, bSet ? "flat" : "adaptive" );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "pointerAccelerationProfileAdaptive" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto profile = bSet ? LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE : LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
            auto status = libinput_device_config_accel_set_profile( pDev, profile );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: accel profile = %s", pszSysName, bSet ? "adaptive" : "flat" );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "naturalScroll" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto status = libinput_device_config_scroll_set_natural_scroll_enabled( pDev, bSet );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: naturalScroll = %d", pszSysName, bSet );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "scrollFactor" ) == 0 )
        {
            double flFactor = 1.0;
            int r = sd_bus_message_read( value, "d", &flFactor );
            if ( r < 0 ) return r;
            pSelf->SetScrollFactor( pszSysName, flFactor );
            log_dbus.infof( "[cloud-mouse] %s: scrollFactor = %.2f", pszSysName, flFactor );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "leftHanded" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto status = libinput_device_config_left_handed_set( pDev, bSet );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: leftHanded = %d", pszSysName, bSet );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "middleEmulation" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto cfg = bSet ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
            auto status = libinput_device_config_middle_emulation_set_enabled( pDev, cfg );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: middleEmulation = %d", pszSysName, bSet );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        if ( strcmp( property, "enabled" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto mode = bSet ? LIBINPUT_CONFIG_SEND_EVENTS_ENABLED : LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;
            auto status = libinput_device_config_send_events_set_mode( pDev, mode );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: enabled = %d", pszSysName, bSet );
            return 0;
        }

        if ( strcmp( property, "scrollOnButtonDown" ) == 0 )
        {
            int bSet = 0;
            int r = sd_bus_message_read( value, "b", &bSet );
            if ( r < 0 ) return r;
            auto method = bSet ? LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN : LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
            auto status = libinput_device_config_scroll_set_method( pDev, method );
            if ( status != LIBINPUT_CONFIG_STATUS_SUCCESS )
                return sd_bus_error_setf( error, SD_BUS_ERROR_FAILED, "libinput error: %d", (int)status );
            log_dbus.infof( "[cloud-mouse] %s: scrollOnButtonDown = %d", pszSysName, bSet );
            pSelf->SaveDeviceConfig( pszSysName, pDev );
            return 0;
        }

        return sd_bus_error_setf( error, SD_BUS_ERROR_PROPERTY_READ_ONLY, "Property %s is read-only", property );
    }

    // ========================================================================
    // sd-bus vtable: org.gamescope.InputDeviceManager
    // ========================================================================

    int CInputDeviceDBus::ManagerPropertyGet(
        sd_bus *bus, const char *path, const char *interface,
        const char *property, sd_bus_message *reply,
        void *userdata, sd_bus_error *error )
    {
        auto *pSelf = static_cast<CInputDeviceDBus*>( userdata );

        if ( strcmp( property, "devicesSysNames" ) == 0 )
        {
            auto names = pSelf->GetDeviceSysNames();
            int r = sd_bus_message_open_container( reply, 'a', "s" );
            if ( r < 0 ) return r;
            for ( auto &name : names )
            {
                r = sd_bus_message_append( reply, "s", name.c_str() );
                if ( r < 0 ) return r;
            }
            return sd_bus_message_close_container( reply );
        }

        return sd_bus_error_setf( error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property: %s", property );
    }

    // ========================================================================
    // vtable definitions
    // ========================================================================

    // Per-device interface: org.gamescope.InputDevice
    // Все properties используют custom getter/setter для единообразия.
    //
    // SD_BUS_WRITABLE_PROPERTY использует два callback: get и set.
    // SD_BUS_PROPERTY использует только get.
    //
    // Формат: name, signature, getter, setter, offset, flags

    #define DEV_RO_PROP_S(name) \
        SD_BUS_PROPERTY(name, "s", DevicePropertyGet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)
    #define DEV_RO_PROP_B(name) \
        SD_BUS_PROPERTY(name, "b", DevicePropertyGet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)
    #define DEV_RO_PROP_D(name) \
        SD_BUS_PROPERTY(name, "d", DevicePropertyGet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)
    #define DEV_RO_PROP_I(name) \
        SD_BUS_PROPERTY(name, "i", DevicePropertyGet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)
    #define DEV_RW_PROP_B(name) \
        SD_BUS_WRITABLE_PROPERTY(name, "b", DevicePropertyGet, DevicePropertySet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)
    #define DEV_RW_PROP_D(name) \
        SD_BUS_WRITABLE_PROPERTY(name, "d", DevicePropertyGet, DevicePropertySet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE)

    const sd_bus_vtable CInputDeviceDBus::s_DeviceVTable[] =
    {
        SD_BUS_VTABLE_START( 0 ),
        // General
        DEV_RO_PROP_S( "name" ),
        DEV_RO_PROP_S( "sysName" ),
        DEV_RO_PROP_B( "pointer" ),
        DEV_RO_PROP_B( "keyboard" ),
        DEV_RO_PROP_B( "touchpad" ),
        DEV_RO_PROP_B( "touch" ),
        DEV_RO_PROP_B( "supportsDisableEvents" ),
        DEV_RW_PROP_B( "enabled" ),
        DEV_RO_PROP_I( "supportedButtons" ),
        // Acceleration
        DEV_RO_PROP_B( "supportsPointerAcceleration" ),
        DEV_RO_PROP_D( "defaultPointerAcceleration" ),
        DEV_RW_PROP_D( "pointerAcceleration" ),
        DEV_RO_PROP_B( "supportsPointerAccelerationProfileFlat" ),
        DEV_RO_PROP_B( "defaultPointerAccelerationProfileFlat" ),
        DEV_RW_PROP_B( "pointerAccelerationProfileFlat" ),
        DEV_RO_PROP_B( "supportsPointerAccelerationProfileAdaptive" ),
        DEV_RO_PROP_B( "defaultPointerAccelerationProfileAdaptive" ),
        DEV_RW_PROP_B( "pointerAccelerationProfileAdaptive" ),
        // Natural scroll
        DEV_RO_PROP_B( "supportsNaturalScroll" ),
        DEV_RO_PROP_B( "naturalScrollEnabledByDefault" ),
        DEV_RW_PROP_B( "naturalScroll" ),
        // Scroll factor
        DEV_RW_PROP_D( "scrollFactor" ),
        // Left-handed
        DEV_RO_PROP_B( "supportsLeftHanded" ),
        DEV_RO_PROP_B( "leftHandedEnabledByDefault" ),
        DEV_RW_PROP_B( "leftHanded" ),
        // Middle emulation
        DEV_RO_PROP_B( "supportsMiddleEmulation" ),
        DEV_RO_PROP_B( "middleEmulationEnabledByDefault" ),
        DEV_RW_PROP_B( "middleEmulation" ),
        // Scroll on button down
        DEV_RO_PROP_B( "supportsScrollOnButtonDown" ),
        DEV_RO_PROP_B( "scrollOnButtonDownEnabledByDefault" ),
        DEV_RW_PROP_B( "scrollOnButtonDown" ),
        SD_BUS_VTABLE_END,
    };

    #undef DEV_RO_PROP_S
    #undef DEV_RO_PROP_B
    #undef DEV_RO_PROP_D
    #undef DEV_RO_PROP_I
    #undef DEV_RW_PROP_B
    #undef DEV_RW_PROP_D

    // Manager interface: org.gamescope.InputDeviceManager
    const sd_bus_vtable CInputDeviceDBus::s_ManagerVTable[] =
    {
        SD_BUS_VTABLE_START( 0 ),
        SD_BUS_PROPERTY( "devicesSysNames", "as", ManagerPropertyGet, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE ),
        SD_BUS_SIGNAL( "deviceAdded", "s", 0 ),
        SD_BUS_SIGNAL( "deviceRemoved", "s", 0 ),
        SD_BUS_VTABLE_END,
    };

    // ========================================================================
    // CInputDeviceDBus lifecycle
    // ========================================================================

    CInputDeviceDBus::CInputDeviceDBus()
    {
    }

    CInputDeviceDBus::~CInputDeviceDBus()
    {
        Shutdown();
    }

    bool CInputDeviceDBus::Init()
    {
        int r = sd_bus_open_user( &m_pBus );
        if ( r < 0 )
        {
            log_dbus.errorf( "Failed to connect to session bus: %s", strerror( -r ) );
            return false;
        }

        // Register manager interface
        r = sd_bus_add_object_vtable( m_pBus, &m_pManagerSlot,
            "/org/gamescope/Input",
            "org.gamescope.InputDeviceManager",
            s_ManagerVTable,
            this );
        if ( r < 0 )
        {
            log_dbus.errorf( "Failed to register manager vtable: %s", strerror( -r ) );
            return false;
        }

        // Request bus name
        r = sd_bus_request_name( m_pBus, "org.gamescope.Input", 0 );
        if ( r < 0 )
        {
            log_dbus.errorf( "Failed to acquire bus name: %s", strerror( -r ) );
            return false;
        }

        log_dbus.infof( "[cloud-mouse] D-Bus service org.gamescope.Input registered" );
        return true;
    }

    void CInputDeviceDBus::Shutdown()
    {
        if ( m_pBus )
        {
            sd_bus_release_name( m_pBus, "org.gamescope.Input" );

            // Unregister all device vtables
            {
                std::lock_guard<std::mutex> lock( m_mutex );
                for ( auto &[name, info] : m_devices )
                {
                    if ( info.pSlot )
                        sd_bus_slot_unref( info.pSlot );
                    if ( info.pDevice )
                        libinput_device_unref( info.pDevice );
                }
                m_devices.clear();
            }

            if ( m_pManagerSlot )
            {
                sd_bus_slot_unref( m_pManagerSlot );
                m_pManagerSlot = nullptr;
            }

            sd_bus_unref( m_pBus );
            m_pBus = nullptr;
        }
    }

    int CInputDeviceDBus::GetFD()
    {
        if ( !m_pBus )
            return -1;
        return sd_bus_get_fd( m_pBus );
    }

    void CInputDeviceDBus::OnPollIn()
    {
        if ( !m_pBus )
            return;

        // Process pending D-Bus messages
        for ( ;; )
        {
            int r = sd_bus_process( m_pBus, nullptr );
            if ( r < 0 )
            {
                log_dbus.errorf( "sd_bus_process failed: %s", strerror( -r ) );
                break;
            }
            if ( r == 0 )
                break; // No more to process
        }
    }

    // ========================================================================
    // Device management
    // ========================================================================

    bool CInputDeviceDBus::RegisterDevice( libinput_device *pDevice )
    {
        const char *pszSysName = libinput_device_get_sysname( pDevice );
        if ( !pszSysName )
            return false;

        std::string sSysName( pszSysName );
        std::string sPath = "/org/gamescope/Input/Device/" + sSysName;

        sd_bus_slot *pSlot = nullptr;
        int r = sd_bus_add_object_vtable( m_pBus, &pSlot,
            sPath.c_str(),
            "org.gamescope.InputDevice",
            s_DeviceVTable,
            this );
        if ( r < 0 )
        {
            log_dbus.errorf( "Failed to register device %s: %s", pszSysName, strerror( -r ) );
            return false;
        }

        libinput_device_ref( pDevice );

        {
            std::lock_guard<std::mutex> lock( m_mutex );
            m_devices[sSysName] = DeviceInfo{ pDevice, pSlot, kDefaultScrollFactor };
        }

        return true;
    }

    void CInputDeviceDBus::UnregisterDevice( const std::string &sSysName )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_devices.find( sSysName );
        if ( it == m_devices.end() )
            return;

        if ( it->second.pSlot )
            sd_bus_slot_unref( it->second.pSlot );
        if ( it->second.pDevice )
            libinput_device_unref( it->second.pDevice );

        m_devices.erase( it );
    }

    void CInputDeviceDBus::OnDeviceAdded( libinput_device *pDevice )
    {
        const char *pszSysName = libinput_device_get_sysname( pDevice );
        const char *pszName = libinput_device_get_name( pDevice );
        if ( !pszSysName )
            return;

        if ( RegisterDevice( pDevice ) )
        {
            log_dbus.infof( "[cloud-mouse] Device added: %s (%s)", pszName ? pszName : "unknown", pszSysName );

            // Загружаем сохранённые настройки
            LoadDeviceConfig( pszSysName, pDevice );

            // Emit D-Bus signal
            if ( m_pBus )
            {
                sd_bus_emit_signal( m_pBus,
                    "/org/gamescope/Input",
                    "org.gamescope.InputDeviceManager",
                    "deviceAdded",
                    "s", pszSysName );
            }
        }
    }

    void CInputDeviceDBus::OnDeviceRemoved( libinput_device *pDevice )
    {
        const char *pszSysName = libinput_device_get_sysname( pDevice );
        if ( !pszSysName )
            return;

        const char *pszName = libinput_device_get_name( pDevice );
        log_dbus.infof( "[cloud-mouse] Device removed: %s (%s)", pszName ? pszName : "unknown", pszSysName );

        std::string sSysName( pszSysName );
        UnregisterDevice( sSysName );

        // Emit D-Bus signal
        if ( m_pBus )
        {
            sd_bus_emit_signal( m_pBus,
                "/org/gamescope/Input",
                "org.gamescope.InputDeviceManager",
                "deviceRemoved",
                "s", pszSysName );
        }
    }

    libinput_device *CInputDeviceDBus::FindDevice( const char *pszSysName )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_devices.find( pszSysName );
        if ( it == m_devices.end() )
            return nullptr;
        return it->second.pDevice;
    }

    std::vector<std::string> CInputDeviceDBus::GetDeviceSysNames()
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        std::vector<std::string> names;
        names.reserve( m_devices.size() );
        for ( auto &[name, info] : m_devices )
            names.push_back( name );
        return names;
    }

    double CInputDeviceDBus::GetScrollFactor( const char *pszSysName )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_devices.find( pszSysName );
        if ( it == m_devices.end() )
            return 1.0;
        return it->second.flScrollFactor;
    }

    void CInputDeviceDBus::SetScrollFactor( const char *pszSysName, double flFactor )
    {
        std::lock_guard<std::mutex> lock( m_mutex );
        auto it = m_devices.find( pszSysName );
        if ( it != m_devices.end() )
            it->second.flScrollFactor = flFactor;
    }

    // ========================================================================
    // Персистентность: save/load в ~/.config/gamescope-input.conf
    // Формат INI-подобный: [device_sysname]
    // key=value
    // ========================================================================

    static std::string GetConfigPath()
    {
        const char *pszHome = getenv( "HOME" );
        if ( !pszHome )
            pszHome = "/tmp";
        std::string sDir = std::string( pszHome ) + "/.config";
        std::filesystem::create_directories( sDir );
        return sDir + "/gamescope-input.conf";
    }

    // Парсер: читаем весь файл в map<section, map<key,value>>
    using ConfigMap = std::map<std::string, std::map<std::string, std::string>>;

    static ConfigMap ReadConfigFile()
    {
        ConfigMap cfg;
        std::ifstream f( GetConfigPath() );
        if ( !f.is_open() )
            return cfg;

        std::string sSection;
        std::string sLine;
        while ( std::getline( f, sLine ) )
        {
            // Trim
            while ( !sLine.empty() && isspace( sLine.front() ) ) sLine.erase( sLine.begin() );
            while ( !sLine.empty() && isspace( sLine.back() ) ) sLine.pop_back();
            if ( sLine.empty() || sLine[0] == '#' )
                continue;

            if ( sLine.front() == '[' && sLine.back() == ']' )
            {
                sSection = sLine.substr( 1, sLine.size() - 2 );
                continue;
            }

            auto nEq = sLine.find( '=' );
            if ( nEq == std::string::npos || sSection.empty() )
                continue;

            std::string sKey = sLine.substr( 0, nEq );
            std::string sVal = sLine.substr( nEq + 1 );
            cfg[sSection][sKey] = sVal;
        }
        return cfg;
    }

    static void WriteConfigFile( const ConfigMap &cfg )
    {
        std::ofstream f( GetConfigPath(), std::ios::trunc );
        if ( !f.is_open() )
            return;

        f << "# Gamescope Cloud Mouse settings — auto-generated\n";
        for ( auto &[section, props] : cfg )
        {
            f << "\n[" << section << "]\n";
            for ( auto &[key, val] : props )
                f << key << "=" << val << "\n";
        }
    }

    void CInputDeviceDBus::SaveDeviceConfig( const char *pszSysName, libinput_device *pDev )
    {
        ConfigMap cfg = ReadConfigFile();
        auto &sec = cfg[pszSysName];

        // libinput properties
        sec["pointerAcceleration"] = std::to_string( libinput_device_config_accel_get_speed( pDev ) );

        auto profile = libinput_device_config_accel_get_profile( pDev );
        sec["accelProfile"] = ( profile == LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT ) ? "flat" : "adaptive";

        sec["naturalScroll"] = std::to_string( (int)libinput_device_config_scroll_get_natural_scroll_enabled( pDev ) );
        sec["leftHanded"] = std::to_string( (int)libinput_device_config_left_handed_get( pDev ) );
        sec["middleEmulation"] = std::to_string(
            (int)( libinput_device_config_middle_emulation_get_enabled( pDev ) == LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED ) );

        // in-memory property
        sec["scrollFactor"] = std::to_string( GetScrollFactor( pszSysName ) );

        WriteConfigFile( cfg );
        log_dbus.infof( "[cloud-mouse] Config saved for %s", pszSysName );
    }

    void CInputDeviceDBus::LoadDeviceConfig( const char *pszSysName, libinput_device *pDev )
    {
        ConfigMap cfg = ReadConfigFile();
        auto it = cfg.find( pszSysName );
        if ( it == cfg.end() )
        {
            log_dbus.infof( "[cloud-mouse] No saved config for %s, using defaults", pszSysName );
            return;
        }

        auto &sec = it->second;

        auto findVal = [&]( const char *key ) -> const std::string* {
            auto vit = sec.find( key );
            return ( vit != sec.end() ) ? &vit->second : nullptr;
        };

        if ( auto *v = findVal( "pointerAcceleration" ) )
        {
            double accel = std::stod( *v );
            libinput_device_config_accel_set_speed( pDev, accel );
            log_dbus.infof( "[cloud-mouse] %s: restored pointerAcceleration = %.3f", pszSysName, accel );
        }

        if ( auto *v = findVal( "accelProfile" ) )
        {
            auto profile = ( *v == "flat" ) ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            libinput_device_config_accel_set_profile( pDev, profile );
            log_dbus.infof( "[cloud-mouse] %s: restored accelProfile = %s", pszSysName, v->c_str() );
        }

        if ( auto *v = findVal( "naturalScroll" ) )
        {
            libinput_device_config_scroll_set_natural_scroll_enabled( pDev, std::stoi( *v ) );
            log_dbus.infof( "[cloud-mouse] %s: restored naturalScroll = %s", pszSysName, v->c_str() );
        }

        if ( auto *v = findVal( "leftHanded" ) )
        {
            libinput_device_config_left_handed_set( pDev, std::stoi( *v ) );
            log_dbus.infof( "[cloud-mouse] %s: restored leftHanded = %s", pszSysName, v->c_str() );
        }

        if ( auto *v = findVal( "middleEmulation" ) )
        {
            auto cfg_val = std::stoi( *v ) ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED;
            libinput_device_config_middle_emulation_set_enabled( pDev, cfg_val );
            log_dbus.infof( "[cloud-mouse] %s: restored middleEmulation = %s", pszSysName, v->c_str() );
        }

        if ( auto *v = findVal( "scrollFactor" ) )
        {
            double factor = std::stod( *v );
            SetScrollFactor( pszSysName, factor );
            log_dbus.infof( "[cloud-mouse] %s: restored scrollFactor = %.2f", pszSysName, factor );
        }

        log_dbus.infof( "[cloud-mouse] Config loaded for %s", pszSysName );
    }
}
