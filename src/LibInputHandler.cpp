#include "LibInputHandler.h"
#include "InputDeviceDBus.h"

#include <libinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <map>
#include <string>

#include "log.hpp"
#include "backend.h"
#include "wlserver.hpp"
#include "Utils/Defer.h"

// Handles libinput in contexts where we don't have a session
// and can't use the wlroots libinput stuff.
//
// eg. in VR where we want global access to the m + kb
// without doing any seat dance.
//
// That may change in the future...
// but for now, this solves that problem.

namespace gamescope
{
    static LogScope log_input_stealer( "InputStealer" );

    const libinput_interface CLibInputHandler::s_LibInputInterface =
    {
        .open_restricted = []( const char *pszPath, int nFlags, void *pUserData ) -> int
        {
            return open( pszPath, nFlags );
        },

        .close_restricted = []( int nFd, void *pUserData ) -> void
        {
            close( nFd );
        },
    };

    CLibInputHandler::CLibInputHandler()
    {
    }

    CLibInputHandler::~CLibInputHandler()
    {
        if ( m_pLibInput )
        {
            libinput_unref( m_pLibInput );
            m_pLibInput = nullptr;
        }

        if ( m_pUdev )
        {
            udev_unref( m_pUdev );
            m_pUdev = nullptr;
        }
    }

    bool CLibInputHandler::Init()
    {
        m_pUdev = udev_new();
        if ( !m_pUdev )
        {
            log_input_stealer.errorf( "Failed to create udev interface" );
            return false;
        }

        m_pLibInput = libinput_udev_create_context( &s_LibInputInterface, nullptr, m_pUdev );
        if ( !m_pLibInput )
        {
            log_input_stealer.errorf( "Failed to create libinput context" );
            return false;
        }

        const char *pszSeatName = "seat0";
        if ( libinput_udev_assign_seat( m_pLibInput, pszSeatName ) == -1 )
        {
            log_input_stealer.errorf( "Could not assign seat \"%s\"", pszSeatName );
            return false;
        }

        return true;
    }

    int CLibInputHandler::GetFD()
    {
        if ( !m_pLibInput )
            return -1;

        return libinput_get_fd( m_pLibInput );
    }

    void CLibInputHandler::OnPollIn()
    {
        static uint32_t s_uSequence = 0;

        // [libinput-diag] Счётчики по устройствам
        struct DevStats { uint32_t rel = 0; uint32_t abs = 0; };
        static std::map<std::string, DevStats> s_devStats;
        static auto s_tLastReport = std::chrono::steady_clock::now();

        libinput_dispatch( m_pLibInput );

        // [sunshine] Input oversampling x2:
        // Mouse motion → lockfree atomic (VBlank timer забирает каждые 8ms).
        // Buttons/keys/scroll → batched с lock (не теряем дискретные события).
        double flAccumDx = 0.0, flAccumDy = 0.0;
        bool bHasMotion = false;
        double flAbsX = 0.0, flAbsY = 0.0;
        bool bHasAbsMotion = false;

        struct ButtonEvent { uint32_t button; bool pressed; };
        struct KeyEvent { uint32_t key; bool pressed; };
        std::vector<ButtonEvent> buttons;
        std::vector<KeyEvent> keys;

		while ( libinput_event *pEvent = libinput_get_event( m_pLibInput ) )
        {
            defer( libinput_event_destroy( pEvent ) );

            libinput_event_type eEventType = libinput_event_get_type( pEvent );

            // [libinput-diag] Имя устройства для pointer events
            if ( eEventType == LIBINPUT_EVENT_POINTER_MOTION ||
                 eEventType == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE )
            {
                libinput_device *pDev = libinput_event_get_device( pEvent );
                const char *pName = pDev ? libinput_device_get_name( pDev ) : "unknown";
                std::string sName( pName ? pName : "unknown" );
                if ( eEventType == LIBINPUT_EVENT_POINTER_MOTION )
                    s_devStats[sName].rel++;
                else
                    s_devStats[sName].abs++;
            }

            switch ( eEventType )
            {
                case LIBINPUT_EVENT_POINTER_MOTION:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );
                    flAccumDx += libinput_event_pointer_get_dx( pPointerEvent );
                    flAccumDy += libinput_event_pointer_get_dy( pPointerEvent );
                    bHasMotion = true;
                }
                break;

                case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );
                    flAbsX = libinput_event_pointer_get_absolute_x( pPointerEvent );
                    flAbsY = libinput_event_pointer_get_absolute_y( pPointerEvent );
                    bHasAbsMotion = true;
                }
                break;

                case LIBINPUT_EVENT_POINTER_BUTTON:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );
                    uint32_t uButton = libinput_event_pointer_get_button( pPointerEvent );
                    libinput_button_state eButtonState = libinput_event_pointer_get_button_state( pPointerEvent );
                    buttons.push_back( { uButton, eButtonState == LIBINPUT_BUTTON_STATE_PRESSED } );
                }
                break;

                case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
                {
                    libinput_event_pointer *pPointerEvent = libinput_event_get_pointer_event( pEvent );

                    // Получаем scrollFactor для этого устройства
                    double flFactor = 1.0;
                    if ( m_pDBus )
                    {
                        libinput_device *pDev = libinput_event_get_device( pEvent );
                        const char *pszSysName = pDev ? libinput_device_get_sysname( pDev ) : nullptr;
                        if ( pszSysName )
                            flFactor = m_pDBus->GetScrollFactor( pszSysName );
                    }

                    static constexpr libinput_pointer_axis eAxes[] =
                    {
                        LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
                        LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                    };

                    for ( uint32_t i = 0; i < std::size( eAxes ); i++ )
                    {
                        libinput_pointer_axis eAxis = eAxes[i];
                        if ( !libinput_event_pointer_has_axis( pPointerEvent, eAxis ) )
                            continue;
                        double flScroll = libinput_event_pointer_get_scroll_value_v120( pPointerEvent, eAxis );
                        m_flScrollAccum[i] += ( flScroll / 120.0 ) * flFactor;
                    }
                }
                break;

                case LIBINPUT_EVENT_KEYBOARD_KEY:
                {
                    libinput_event_keyboard *pKeyboardEvent = libinput_event_get_keyboard_event( pEvent );
                    uint32_t uKey = libinput_event_keyboard_get_key( pKeyboardEvent );
                    libinput_key_state eState = libinput_event_keyboard_get_key_state( pKeyboardEvent );
                    keys.push_back( { uKey, eState == LIBINPUT_KEY_STATE_PRESSED } );
                }
                break;

                case LIBINPUT_EVENT_DEVICE_ADDED:
                {
                    libinput_device *pDev = libinput_event_get_device( pEvent );
                    if ( m_pDBus )
                        m_pDBus->OnDeviceAdded( pDev );
                }
                break;

                case LIBINPUT_EVENT_DEVICE_REMOVED:
                {
                    libinput_device *pDev = libinput_event_get_device( pEvent );
                    if ( m_pDBus )
                        m_pDBus->OnDeviceRemoved( pDev );
                }
                break;

                default:
                    break;
            }
		}

        // [sunshine] Direct path: motion → KWin напрямую.
        // Задержка ~0.3ms вместо ~9ms (было: atomic → VBlank Timer batch → flush).
        // BT-мышь на клиенте была реальной причиной зависания, а не частота events.
        if ( bHasMotion )
        {
            GetBackend()->NotifyPhysicalInput( InputType::Mouse );

            wlserver_lock();
            wlserver_mousemotion( flAccumDx, flAccumDy, ++s_uSequence );
            wlserver_unlock();
        }

        // Abs motion: прямой вызов wlserver_mousewarp
        if ( bHasAbsMotion )
        {
            GetBackend()->NotifyPhysicalInput( InputType::Mouse );

            wlserver_lock();
            wlserver_mousewarp( flAbsX, flAbsY, ++s_uSequence, true );
            wlserver_unlock();
        }

        // [sunshine] Discrete events (buttons, keys, scroll): batched с lock.
        // Эти события нельзя терять — каждое press/release значимо.
        bool bHasDiscreteInput = !buttons.empty() || !keys.empty()
            || m_flScrollAccum[0] != 0.0 || m_flScrollAccum[1] != 0.0;

        if ( bHasDiscreteInput )
        {
            wlserver_lock();

            for ( auto &btn : buttons )
                wlserver_mousebutton( btn.button, btn.pressed, ++s_uSequence );

            for ( auto &key : keys )
                wlserver_key( key.key, key.pressed, ++s_uSequence );

            if ( m_flScrollAccum[0] != 0.0 || m_flScrollAccum[1] != 0.0 )
            {
                wlserver_mousewheel( m_flScrollAccum[0], m_flScrollAccum[1], ++s_uSequence );
                m_flScrollAccum[0] = 0.0;
                m_flScrollAccum[1] = 0.0;
            }

            wlserver_unlock();
        }

        // [libinput-diag] Периодический отчёт каждые 5 секунд
        auto tNow = std::chrono::steady_clock::now();
        auto nElapsed = std::chrono::duration_cast<std::chrono::seconds>( tNow - s_tLastReport ).count();
        if ( nElapsed >= 5 && !s_devStats.empty() )
        {
            for ( auto &[name, stats] : s_devStats )
            {
                if ( stats.rel > 0 || stats.abs > 0 )
                {
                    log_input_stealer.infof( "[libinput-diag] 5s '%s': rel=%u abs=%u",
                        name.c_str(), stats.rel, stats.abs );
                }
            }
            s_devStats.clear();
            s_tLastReport = tNow;
        }
    }
}
