// Sunshine Backend для Cloud Desktop
// Кастомный backend Gamescope для стриминга через Sunshine screencopy.
// Фаза 4: GBM Pipeline + Vulkan render + Screencopy.

#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"
#include "log.hpp"
#include "LibInputHandler.h"
#if HAVE_LIBSYSTEMD
#include "InputDeviceDBus.h"
#endif

// Heartbeat: timestamp последнего Present() для auto-recovery из чёрного экрана.
// VBlank timer проверяет: если >500ms без Present → force repaint.
std::atomic<std::chrono::steady_clock::time_point> g_tLastSunshinePresent{
	std::chrono::steady_clock::now()
};

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <chrono>
#include <mutex>
#include <cstring>

// wlroots unstable interfaces (C headers → extern "C")
extern "C" {
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/dmabuf.h>
}

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

// [sunshine] PPM dump by request from steamcompmgr cursor test
extern std::atomic<int> g_nSunshineDumpRequest;
extern char g_szSunshineDumpName[128];

namespace gamescope
{

// ─── GBM Slot: wlr_buffer wrapper вокруг gbm_bo ───

static constexpr int kGBMSlotCount = 3;

struct GBMSlot
{
    struct wlr_buffer base;              // wlr_buffer наследование (первый член!)
    struct gbm_bo *bo = nullptr;
    int dma_fd = -1;
    uint32_t stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t modifier = 0;
    gamescope::OwningRc<CVulkanTexture> vulkanTex;
    std::atomic<bool> in_use{false};
};

// wlr_buffer_impl callbacks
static void gbm_slot_destroy( struct wlr_buffer *buffer )
{
    GBMSlot *slot = wl_container_of( buffer, slot, base );
    // Не уничтожаем — время жизни управляется slot pool
    // Просто отмечаем как свободный
    slot->in_use.store( false );
}

static bool gbm_slot_get_dmabuf( struct wlr_buffer *buffer,
                                  struct wlr_dmabuf_attributes *attribs )
{
    GBMSlot *slot = wl_container_of( buffer, slot, base );
    memset( attribs, 0, sizeof(*attribs) );
    attribs->width = slot->width;
    attribs->height = slot->height;
    attribs->format = DRM_FORMAT_ARGB8888;
    attribs->modifier = slot->modifier;
    attribs->n_planes = 1;
    attribs->fd[0] = slot->dma_fd;
    attribs->stride[0] = slot->stride;
    attribs->offset[0] = 0;
    return true;
}

const struct wlr_buffer_impl gbm_slot_buffer_impl = {
    .destroy = gbm_slot_destroy,
    .get_dmabuf = gbm_slot_get_dmabuf,
};

// Helper: извлечь оригинальный CVulkanTexture из GBMSlot (без re-import DMA-BUF)
CVulkanTexture *sunshine_get_buffer_vulkan_tex( struct wlr_buffer *buf )
{
    if ( !buf || buf->impl != &gbm_slot_buffer_impl )
        return nullptr;
    GBMSlot *slot = wl_container_of( buf, slot, base );
    return slot->vulkanTex.get();
}

// ─── CSunshineConnector ───

    class CSunshineConnector final : public CBaseBackendConnector
    {
    public:
        CSunshineConnector()
        {
        }
        virtual ~CSunshineConnector()
        {
        }

        virtual gamescope::GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override
        {
            return false;
        }
        virtual bool IsHDRActive() const override
        {
            return false;
        }
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override
        {
            return m_HDRInfo;
        }
		virtual bool IsVRRActive() const override
		{
			return false;
		}
        virtual std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }

        virtual bool SupportsVRR() const override
        {
            return false;
        }

        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{};
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
			*displayColorimetry = displaycolorimetry_709;
			*displayEOTF = EOTF_Gamma22;
			*outputEncodingColorimetry = displaycolorimetry_709;
			*outputEncodingEOTF = EOTF_Gamma22;
        }

        virtual const char *GetName() const override
        {
            return "Sunshine";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Sunshine Virtual Display";
        }

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override;

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
    };

// ─── CSunshineBackend ───

	class CSunshineBackend final : public CBaseBackend
	{
	public:
		CSunshineBackend()
			: m_LibInputWaiter{ "gamescope-libinput" }
		{
		}

		virtual ~CSunshineBackend()
		{
			// Cleanup GBM slots
			for ( int i = 0; i < kGBMSlotCount; i++ )
			{
				if ( m_Slots[i].dma_fd >= 0 )
					close( m_Slots[i].dma_fd );
				if ( m_Slots[i].bo )
					gbm_bo_destroy( m_Slots[i].bo );
			}
			if ( m_pGBMDevice )
				gbm_device_destroy( m_pGBMDevice );
			if ( m_nRenderFd >= 0 )
				close( m_nRenderFd );
		}

		virtual bool Init() override
		{
            fprintf( stderr, "[sunshine-backend] Init: starting Sunshine backend\n" );

			g_nOutputWidth = g_nPreferredOutputWidth;
			g_nOutputHeight = g_nPreferredOutputHeight;
			g_nOutputRefresh = g_nNestedRefresh;

			if ( g_nOutputHeight == 0 )
			{
				if ( g_nOutputWidth != 0 )
				{
					fprintf( stderr, "[sunshine-backend] Cannot specify -W without -H\n" );
					return false;
				}
				g_nOutputHeight = 1080;
			}
			if ( g_nOutputWidth == 0 )
				g_nOutputWidth = g_nOutputHeight * 16 / 9;
			if ( g_nOutputRefresh == 0 )
				g_nOutputRefresh = ConvertHztomHz( 60 );

            fprintf( stderr, "[sunshine-backend] Init: resolution %dx%d @ %d mHz\n",
                g_nOutputWidth, g_nOutputHeight, g_nOutputRefresh );

			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
			{
                fprintf( stderr, "[sunshine-backend] Init: Vulkan init FAILED\n" );
				return false;
			}
            fprintf( stderr, "[sunshine-backend] Init: Vulkan initialized OK\n" );

			if ( !wlsession_init() )
			{
				fprintf( stderr, "[sunshine-backend] Init: Wayland session init FAILED\n" );
				return false;
			}
            fprintf( stderr, "[sunshine-backend] Init: Wayland session initialized OK\n" );

			// ─── GBM Device из render node ───
			if ( !InitGBM() )
			{
				fprintf( stderr, "[sunshine-backend] Init: GBM init FAILED\n" );
				return false;
			}

			// ─── libinput для mouse/keyboard от Sunshine uinput ───
			{
				std::unique_ptr<CLibInputHandler> pLibInput = std::make_unique<CLibInputHandler>();
				if ( pLibInput->Init() )
				{
					m_pLibInput = std::move( pLibInput );
					m_LibInputWaiter.AddWaitable( m_pLibInput.get() );
					fprintf( stderr, "[sunshine-backend] Init: CLibInputHandler started OK\n" );

#if HAVE_LIBSYSTEMD
					// [cloud-mouse] D-Bus сервис для input device properties
					m_pInputDBus = std::make_unique<CInputDeviceDBus>();
					if ( m_pInputDBus->Init() )
					{
						m_pLibInput->SetDBus( m_pInputDBus.get() );
						m_LibInputWaiter.AddWaitable( m_pInputDBus.get() );
						fprintf( stderr, "[sunshine-backend] Init: D-Bus org.gamescope.Input started OK\n" );
					}
					else
					{
						fprintf( stderr, "[sunshine-backend] Init: D-Bus org.gamescope.Input FAILED (non-fatal)\n" );
						m_pInputDBus.reset();
					}
#endif
				}
				else
				{
					fprintf( stderr, "[sunshine-backend] Init: CLibInputHandler FAILED (input disabled)\n" );
				}
			}

			return true;
		}

		virtual bool PostInit() override
		{
            fprintf( stderr, "[sunshine-backend] PostInit: sunshine_output=%p screencopy=%p eventfd=%d\n",
                (void*)wlserver.sunshine_output,
                (void*)wlserver.sunshine_screencopy_manager,
                wlserver.sunshine_eventfd );

			fprintf( stderr, "[sunshine-backend] PostInit: GBM device=%p, %d slots allocated\n",
				(void*)m_pGBMDevice, kGBMSlotCount );

			// Регистрируем callback для release slot из eventfd handler
			wlserver.sunshine_release_func = []( void *slot_ptr ) {
				GBMSlot *slot = (GBMSlot *)slot_ptr;
				slot->in_use.store( false );
			};

			return true;
		}

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
			return std::span<const char *const>{};
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return std::span<const char *const>{};
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
			return VK_IMAGE_LAYOUT_GENERAL;
		}
		virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
		{
			*pPrimaryPlaneFormat = DRM_FORMAT_ARGB8888;
			*pOverlayPlaneFormat = DRM_FORMAT_ARGB8888;
		}
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
			if ( bForceModeset )
				ResizeSlotPool();
		}

		bool ResizeSlotPool()
		{
			fprintf( stderr, "[sunshine-backend] ResizeSlotPool: new size %dx%d\n",
				g_nOutputWidth, g_nOutputHeight );

			// Ждём пока все слоты освободятся (макс ~1с)
			for ( int retry = 0; retry < 100; retry++ )
			{
				bool allFree = true;
				for ( int i = 0; i < kGBMSlotCount; i++ )
				{
					if ( m_Slots[i].in_use.load() )
					{
						allFree = false;
						break;
					}
				}
				if ( allFree ) break;
				usleep( 10000 ); // 10ms
			}

			// Уничтожаем старые слоты
			for ( int i = 0; i < kGBMSlotCount; i++ )
			{
				m_Slots[i].vulkanTex = nullptr;
				if ( m_Slots[i].dma_fd >= 0 )
				{
					close( m_Slots[i].dma_fd );
					m_Slots[i].dma_fd = -1;
				}
				if ( m_Slots[i].bo )
				{
					gbm_bo_destroy( m_Slots[i].bo );
					m_Slots[i].bo = nullptr;
				}
				m_Slots[i].in_use.store( false );
			}

			// Создаём новые с текущими размерами
			for ( int i = 0; i < kGBMSlotCount; i++ )
			{
				if ( !InitSlot( &m_Slots[i], i ) )
				{
					fprintf( stderr, "[sunshine-backend] ResizeSlotPool: InitSlot[%d] FAILED\n", i );
					return false;
				}
			}

			fprintf( stderr, "[sunshine-backend] ResizeSlotPool: done, %d slots at %dx%d\n",
				kGBMSlotCount, g_nOutputWidth, g_nOutputHeight );
			return true;
		}

		virtual bool PollState() override
		{
			return false;
		}

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
			return std::make_shared<BackendBlob>( data );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return new CBaseBackendFb();
		}

		virtual bool UsesModifiers() const override
		{
			return false;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			return std::span<const uint64_t>{};
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return &m_Connector;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
				return &m_Connector;

			return nullptr;
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
			return false;
		}

		virtual bool SupportsTearing() const override
		{
			return false;
		}

		virtual bool UsesVulkanSwapchain() const override
		{
			return false;
		}

        virtual bool IsSessionBased() const override
		{
			return false;
		}

		virtual bool SupportsExplicitSync() const override
		{
			return true;
		}

		virtual bool IsPaused() const override
		{
			return false;
		}

		virtual bool IsVisible() const override
		{
			return true;
		}

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
			return uvecSize;
		}

		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
			return false;
		}

		virtual void HackUpdatePatchedEdid() override
		{
		}

	// ─── GBM slot pool API ───

		GBMSlot *AcquireSlot()
		{
			for ( int i = 0; i < kGBMSlotCount; i++ )
			{
				bool expected = false;
				if ( m_Slots[i].in_use.compare_exchange_strong( expected, true ) )
					return &m_Slots[i];
			}
			return nullptr;  // все заняты
		}

		// Вызывается из Wayland thread при release предыдущего буфера
		void ReleaseSlot( GBMSlot *slot )
		{
			if ( slot )
				slot->in_use.store( false );
		}

		GBMSlot *GetSlots() { return m_Slots; }

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

		bool InitGBM()
		{
			// Открываем render node (без DRM Master!)
			m_nRenderFd = open( "/dev/dri/renderD128", O_RDWR | O_CLOEXEC );
			if ( m_nRenderFd < 0 )
			{
				fprintf( stderr, "[sunshine-backend] InitGBM: failed to open renderD128: %s\n", strerror(errno) );
				return false;
			}

			m_pGBMDevice = gbm_create_device( m_nRenderFd );
			if ( !m_pGBMDevice )
			{
				fprintf( stderr, "[sunshine-backend] InitGBM: gbm_create_device failed\n" );
				close( m_nRenderFd );
				m_nRenderFd = -1;
				return false;
			}
			fprintf( stderr, "[sunshine-backend] InitGBM: GBM device created\n" );

			// Аллоцируем 3 GBM slot'а
			for ( int i = 0; i < kGBMSlotCount; i++ )
			{
				if ( !InitSlot( &m_Slots[i], i ) )
					return false;
			}

			fprintf( stderr, "[sunshine-backend] InitGBM: %d GBM slots allocated (%dx%d ARGB8888)\n",
				kGBMSlotCount, g_nOutputWidth, g_nOutputHeight );

			return true;
		}

		bool InitSlot( GBMSlot *slot, int index )
		{
			slot->width = g_nOutputWidth;
			slot->height = g_nOutputHeight;

			// NVIDIA не поддерживает LINEAR — используем GBM_BO_USE_RENDERING,
			// драйвер сам выберет tiled modifier
			slot->bo = gbm_bo_create(
				m_pGBMDevice,
				slot->width, slot->height,
				GBM_FORMAT_ARGB8888,
				GBM_BO_USE_RENDERING );

			if ( !slot->bo )
			{
				fprintf( stderr, "[sunshine-backend] InitSlot[%d]: gbm_bo_create failed\n", index );
				return false;
			}

			slot->dma_fd = gbm_bo_get_fd( slot->bo );
			slot->stride = gbm_bo_get_stride( slot->bo );
			slot->modifier = gbm_bo_get_modifier( slot->bo );

			if ( slot->dma_fd < 0 )
			{
				fprintf( stderr, "[sunshine-backend] InitSlot[%d]: gbm_bo_get_fd failed\n", index );
				return false;
			}

			// Import GBM bo → VkImage через DMA-BUF
			// bStorage: нужен для vulkan_screenshot() compute shader (imageStore)
			// bTransferSrc: нужен для screencopy vkCmdCopyImage
			CVulkanTexture::createFlags slotTexFlags;
			slotTexFlags.bSampled = true;
			slotTexFlags.bStorage = true;
			slotTexFlags.bTransferSrc = true;

			struct wlr_dmabuf_attributes dmabuf = {};
			dmabuf.width = slot->width;
			dmabuf.height = slot->height;
			dmabuf.format = DRM_FORMAT_ARGB8888;
			dmabuf.modifier = slot->modifier;
			dmabuf.n_planes = 1;
			dmabuf.fd[0] = slot->dma_fd;
			dmabuf.stride[0] = slot->stride;
			dmabuf.offset[0] = 0;

			slot->vulkanTex = vulkan_create_texture_from_dmabuf( &dmabuf, nullptr, slotTexFlags );
			if ( !slot->vulkanTex )
			{
				fprintf( stderr, "[sunshine-backend] InitSlot[%d]: vulkan_create_texture_from_dmabuf failed\n", index );
				return false;
			}

			// Инициализируем wlr_buffer wrapper
			wlr_buffer_init( &slot->base, &gbm_slot_buffer_impl, slot->width, slot->height );

			slot->in_use.store( false );

			fprintf( stderr, "[sunshine-backend] InitSlot[%d]: GBM bo=%p fd=%d stride=%u modifier=0x%lx vulkanTex=%p\n",
				index, (void*)slot->bo, slot->dma_fd, slot->stride,
				(unsigned long)slot->modifier, (void*)slot->vulkanTex.get() );

			return true;
		}

        CSunshineConnector m_Connector;
		int m_nRenderFd = -1;
		struct gbm_device *m_pGBMDevice = nullptr;
		GBMSlot m_Slots[kGBMSlotCount];

		// Input bridge: libinput → uinput от Sunshine
		std::shared_ptr<CLibInputHandler> m_pLibInput;
		CAsyncWaiter<CRawPointer<IWaitable>, 16> m_LibInputWaiter;

#if HAVE_LIBSYSTEMD
		// [cloud-mouse] D-Bus сервис для input device properties
		std::unique_ptr<CInputDeviceDBus> m_pInputDBus;
#endif
	};

// ─── Present() implementation ───

int CSunshineConnector::Present( const FrameInfo_t *pFrameInfo, bool bAsync )
{
	CSunshineBackend *pBackend = static_cast<CSunshineBackend*>( GetBackend() );

	// 1. Acquire свободный GBM slot
	GBMSlot *slot = pBackend->AcquireSlot();
	if ( !slot )
	{
		static int s_nDropCount = 0;
		if ( s_nDropCount++ % 60 == 0 )
			fprintf( stderr, "[sunshine-backend] Present: all GBM slots busy, dropped frame %d\n", s_nDropCount );
		return 0;
	}

	// 2. Vulkan render: composit все layers в GBM slot texture
	auto seq = vulkan_screenshot( pFrameInfo, slot->vulkanTex, nullptr );
	if ( !seq )
	{
		pBackend->ReleaseSlot( slot );
		fprintf( stderr, "[sunshine-backend] Present: vulkan_screenshot failed\n" );
		return -1;
	}

	// 3. Ждём GPU completion
	vulkan_wait( *seq, true );

	// 3.5. Frame dump (если запрошен)
	static bool s_bDumpChecked = false;
	static bool s_bDumpEnabled = false;
	static int  s_nDumpCount = 0;
	if ( !s_bDumpChecked )
	{
		s_bDumpChecked = true;
		const char *pDump = getenv( "GAMESCOPE_SUNSHINE_DUMP_FRAME" );
		s_bDumpEnabled = pDump && atoi( pDump ) > 0;
		if ( s_bDumpEnabled )
			fprintf( stderr, "[sunshine-backend] Frame dump ENABLED\n" );
	}

	static auto s_tFirstPresent = std::chrono::steady_clock::now();
	static bool s_bFirstPresent = true;
	if ( s_bFirstPresent ) { s_bFirstPresent = false; s_tFirstPresent = std::chrono::steady_clock::now(); }
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>( std::chrono::steady_clock::now() - s_tFirstPresent ).count();
	if ( s_bDumpEnabled && elapsed >= 10 && s_nDumpCount < 5 )
	{
		s_nDumpCount++;
		// [sunshine] Vulkan readback: gbm_bo_map не работает на NVIDIA tiled буферах.
		// Вместо этого используем встроенный API Gamescope:
		// vulkan_acquire_screenshot_texture → HOST_VISIBLE linear текстура
		// vulkan_screenshot(frameInfo, readbackTex) → повторный рендер в readable формат
		// readbackTex->mappedData() → CPU pointer на пиксели
		auto readbackTex = vulkan_acquire_screenshot_texture(
			slot->width, slot->height, false, DRM_FORMAT_XRGB8888 );
		if ( readbackTex )
		{
			auto seq2 = vulkan_screenshot( pFrameInfo, readbackTex, nullptr );
			if ( seq2 )
			{
				vulkan_wait( *seq2, true );
				uint8_t *data = readbackTex->mappedData();
				if ( data )
				{
					char path[128];
					snprintf( path, sizeof(path), "/tmp/sunshine_frame_%d.ppm", s_nDumpCount );
					FILE *fp = fopen( path, "wb" );
					if ( fp )
					{
						uint32_t pitch = readbackTex->rowPitch();
						fprintf( fp, "P6\n%u %u\n255\n", slot->width, slot->height );
						for ( uint32_t y = 0; y < slot->height; y++ )
						{
							const uint8_t *row = data + y * pitch;
							for ( uint32_t x = 0; x < slot->width; x++ )
							{
								// XRGB8888: B G R X → write R G B
								fputc( row[x*4+2], fp ); // R
								fputc( row[x*4+1], fp ); // G
								fputc( row[x*4+0], fp ); // B
							}
						}
						fclose( fp );
						fprintf( stderr, "[sunshine-backend] Frame dump saved: %s (%ux%u, pitch=%u)\n",
							path, slot->width, slot->height, pitch );
					}
				}
				else
				{
					fprintf( stderr, "[sunshine-backend] Frame dump: mappedData() is NULL\n" );
				}
			}
			else
			{
				fprintf( stderr, "[sunshine-backend] Frame dump: vulkan_screenshot for readback failed\n" );
			}
		}
		else
		{
			fprintf( stderr, "[sunshine-backend] Frame dump: vulkan_acquire_screenshot_texture failed\n" );
		}
	}

	// [sunshine] PPM dump по запросу от cursor test.
	// g_nSunshineDumpRequest и g_szSunshineDumpName устанавливаются из steamcompmgr (global namespace).
	if ( ::g_nSunshineDumpRequest.load() > 0 )
	{
		::g_nSunshineDumpRequest.store( 0 );
		char dumpPath[128];
		strncpy( dumpPath, ::g_szSunshineDumpName, sizeof(dumpPath) );
		dumpPath[sizeof(dumpPath)-1] = '\0';

		auto readbackTex = vulkan_acquire_screenshot_texture(
			slot->width, slot->height, false, DRM_FORMAT_XRGB8888 );
		if ( readbackTex )
		{
			auto seq2 = vulkan_screenshot( pFrameInfo, readbackTex, nullptr );
			if ( seq2 )
			{
				vulkan_wait( *seq2, true );
				uint8_t *data = readbackTex->mappedData();
				if ( data )
				{
					FILE *fp = fopen( dumpPath, "wb" );
					if ( fp )
					{
						uint32_t pitch = readbackTex->rowPitch();
						fprintf( fp, "P6\n%u %u\n255\n", slot->width, slot->height );
						for ( uint32_t y = 0; y < slot->height; y++ )
						{
							const uint8_t *row = data + y * pitch;
							for ( uint32_t x = 0; x < slot->width; x++ )
							{
								fputc( row[x*4+2], fp ); // R
								fputc( row[x*4+1], fp ); // G
								fputc( row[x*4+0], fp ); // B
							}
						}
						fclose( fp );
						fprintf( stderr, "[sunshine-backend] On-demand dump saved: %s (%ux%u)\n",
							dumpPath, slot->width, slot->height );
					}
				}
			}
		}
	}

	// 4. Передаём pending buffer в Wayland thread
	{
		std::lock_guard<std::mutex> lock( wlserver.sunshine_lock );
		// Возвращаем предыдущий pending slot (ещё не committed)
		if ( wlserver.sunshine_pending_slot )
		{
			pBackend->ReleaseSlot( static_cast<GBMSlot*>(wlserver.sunshine_pending_slot) );
		}
		wlserver.sunshine_pending_slot = slot;
		// НЕ делаем wlr_buffer_lock — lifecycle управляется через in_use atomic
	}

	// 5. Signal Wayland thread через eventfd
	if ( wlserver.sunshine_eventfd >= 0 )
	{
		uint64_t val = 1;
		write( wlserver.sunshine_eventfd, &val, sizeof(val) );
	}

	static int s_nPresentCount = 0;
	static int s_nPresentWindow = 0;
	static auto s_tLastFpsReport = std::chrono::steady_clock::now();
	s_nPresentCount++;
	s_nPresentWindow++;

	// [sunshine-diag] FPS отчёт каждые 5 секунд
	auto tNow = std::chrono::steady_clock::now();
	auto nElapsed = std::chrono::duration_cast<std::chrono::milliseconds>( tNow - s_tLastFpsReport ).count();
	if ( nElapsed >= 5000 )
	{
		double fps = (double)s_nPresentWindow * 1000.0 / (double)nElapsed;
		fprintf( stderr, "[sunshine-diag] Present FPS: %.1f (%d frames / %lld ms)\n",
			fps, s_nPresentWindow, (long long)nElapsed );
		s_nPresentWindow = 0;
		s_tLastFpsReport = tNow;
	}
	if ( s_nPresentCount <= 5 || s_nPresentCount % 100 == 0 )
	{
		const char *layerTex = "none";
		void *layerVkImage = nullptr;
		if ( pFrameInfo->layerCount > 0 && pFrameInfo->layers[0].tex )
		{
			layerVkImage = (void*)pFrameInfo->layers[0].tex->vkImage();
		}
		fprintf( stderr, "[sunshine-backend] Present #%d: slot fd=%d slotVk=%p layer0Vk=%p layers=%d\n",
			s_nPresentCount, slot->dma_fd,
			(void*)(slot->vulkanTex ? (void*)slot->vulkanTex->vkImage() : nullptr),
			layerVkImage,
			pFrameInfo->layerCount );
	}
	// Heartbeat: обновляем timestamp для VBlank timer auto-recovery
	g_tLastSunshinePresent.store( std::chrono::steady_clock::now() );

	return 0;
}

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CSunshineBackend>()
	{
		return Set( new CSunshineBackend{} );
	}

}
