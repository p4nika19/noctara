#pragma once

#include <string_view>

// Stable configuration keys. They are part of the config file format: never rename without a migration.
namespace noctara::settings::keys {

inline constexpr std::string_view Language = "general.language";
inline constexpr std::string_view TimeFormat = "general.formatting.time_format";
inline constexpr std::string_view CompactNumbers = "general.formatting.compact_numbers";

inline constexpr std::string_view ThemeMode = "appearance.theme.mode";
inline constexpr std::string_view EnforceContrast = "appearance.theme.enforce_contrast";
inline constexpr std::string_view CustomTheme = "appearance.theme.custom";
inline constexpr std::string_view AccentAdaptive = "appearance.accent.adaptive";
inline constexpr std::string_view AccentColor = "appearance.accent.color";
inline constexpr std::string_view AccentExtraction = "appearance.accent.extraction";
inline constexpr std::string_view BackdropStyle = "appearance.backdrop.style";
inline constexpr std::string_view GlowIntensity = "appearance.glow.intensity";
inline constexpr std::string_view GlowCover = "appearance.glow.cover";
inline constexpr std::string_view GlowControls = "appearance.glow.controls";
inline constexpr std::string_view GlowProgress = "appearance.glow.progress";
inline constexpr std::string_view ButtonMaterial = "appearance.glass.buttons";
inline constexpr std::string_view ButtonShape = "appearance.shape.buttons";
inline constexpr std::string_view ColorTransition = "appearance.motion.color_transition";
inline constexpr std::string_view Density = "layout.content.density";
inline constexpr std::string_view NavPosition = "layout.navigation.position";
inline constexpr std::string_view NavMode = "layout.navigation.mode";
inline constexpr std::string_view NavItems = "layout.navigation.items";
inline constexpr std::string_view PlayerPosition = "layout.player.position";
inline constexpr std::string_view PlayerSize = "layout.player.size";
inline constexpr std::string_view PlayerWaveform = "layout.player.waveform";
inline constexpr std::string_view PlayerTime = "layout.player.time";
inline constexpr std::string_view PlayerVolume = "layout.player.volume";
inline constexpr std::string_view PlayerQueueButton = "layout.player.queue_button";
inline constexpr std::string_view ViewLibrary = "layout.views.library";
inline constexpr std::string_view ViewLikes = "layout.views.likes";
inline constexpr std::string_view ViewHistory = "layout.views.history";
inline constexpr std::string_view ViewCharts = "layout.views.charts";
inline constexpr std::string_view CardSize = "layout.views.card_size";
inline constexpr std::string_view RowPlays = "layout.rows.plays";
inline constexpr std::string_view RowLikes = "layout.rows.likes";
inline constexpr std::string_view RowReposts = "layout.rows.reposts";
inline constexpr std::string_view RowDuration = "layout.rows.duration";
inline constexpr std::string_view FullPlayerArt = "layout.full_player.art_size";
inline constexpr std::string_view FullPlayerQueue = "layout.full_player.queue";
inline constexpr std::string_view FullPlayerWaveform = "layout.full_player.waveform";
inline constexpr std::string_view WindowDecorations = "layout.window.decorations";
inline constexpr std::string_view RememberGeometry = "layout.window.remember_geometry";
inline constexpr std::string_view StartPage = "layout.window.start_page";
inline constexpr std::string_view StartMinimized = "layout.window.start_minimized";
inline constexpr std::string_view CloseToTray = "layout.window.close_to_tray";
inline constexpr std::string_view BackdropAnimate = "appearance.backdrop.animate";
inline constexpr std::string_view GlassStyle = "appearance.glass.style";  // the backdrop blur
inline constexpr std::string_view GlassSurface = "appearance.glass.surface";
inline constexpr std::string_view GlassBlurStrength = "appearance.glass.blur_strength";
inline constexpr std::string_view GlassOpacity = "appearance.glass.opacity";
inline constexpr std::string_view GlassGrain = "appearance.glass.grain";
inline constexpr std::string_view GlassQuality = "appearance.glass.quality";
inline constexpr std::string_view ReduceTransparency = "appearance.glass.reduce_transparency";
inline constexpr std::string_view ReduceMotion = "appearance.motion.reduce";
inline constexpr std::string_view AnimationSpeed = "appearance.motion.speed";
inline constexpr std::string_view CornerRadius = "appearance.shape.corner_radius";
inline constexpr std::string_view CornerRadiusMultiplier = "appearance.shape.corner_radius_multiplier";
inline constexpr std::string_view CoverArtShape = "appearance.shape.cover_art";
inline constexpr std::string_view UiScale = "appearance.text.ui_scale";
inline constexpr std::string_view FontScale = "appearance.text.font_scale";
inline constexpr std::string_view HeadingFont = "appearance.text.heading_font";
inline constexpr std::string_view BodyFont = "appearance.text.body_font";
inline constexpr std::string_view MonoFont = "appearance.text.mono_font";
inline constexpr std::string_view IconWeight = "appearance.icons.weight";

inline constexpr std::string_view StreamQuality = "playback.stream.quality";
inline constexpr std::string_view Crossfade = "playback.transitions.crossfade";
inline constexpr std::string_view RememberVolume = "playback.volume.remember";
inline constexpr std::string_view StartupVolume = "playback.volume.startup";
inline constexpr std::string_view Autoplay = "playback.queue.autoplay";
inline constexpr std::string_view RestoreQueue = "playback.queue.restore";
inline constexpr std::string_view MediaControls = "playback.desktop.media_controls";
inline constexpr std::string_view TrayIcon = "playback.desktop.tray";
inline constexpr std::string_view TrackNotifications = "playback.desktop.notifications";

// One per keyboard action: "keyboard.shortcuts.<action>" (see ui/KeyboardController).
inline constexpr std::string_view ShortcutPrefix = "keyboard.shortcuts.";
inline constexpr std::string_view GlobalShortcuts = "keyboard.global.enabled";

inline constexpr std::string_view GpuAcceleration = "performance.gpu.acceleration";
inline constexpr std::string_view GraphicsApi = "performance.gpu.api";
inline constexpr std::string_view GpuPreference = "performance.gpu.preference";
inline constexpr std::string_view VSync = "performance.rendering.vsync";
inline constexpr std::string_view FrameRateCap = "performance.rendering.frame_rate_cap";
inline constexpr std::string_view EnergySaving = "performance.power.energy_saving";

inline constexpr std::string_view OfflineMode = "network.usage.offline";
inline constexpr std::string_view MeteredMode = "network.usage.metered";
inline constexpr std::string_view BandwidthLimit = "network.usage.bandwidth_limit";
inline constexpr std::string_view DnsMode = "network.dns.mode";
inline constexpr std::string_view DnsServerPreset = "network.dns.server_preset";
inline constexpr std::string_view DnsServers = "network.dns.servers";
inline constexpr std::string_view DohProvider = "network.dns.doh_provider";
inline constexpr std::string_view DohUrl = "network.dns.doh_url";
inline constexpr std::string_view DohBootstrap = "network.dns.doh_bootstrap";
inline constexpr std::string_view DohAllowInsecure = "network.dns.doh_allow_insecure";
inline constexpr std::string_view DnsFallbackToSystem = "network.dns.fallback_to_system";
inline constexpr std::string_view IpFamily = "network.dns.ip_family";
inline constexpr std::string_view HostOverrides = "network.dns.host_overrides";
inline constexpr std::string_view DnsMinTtl = "network.dns.cache.min_ttl";
inline constexpr std::string_view DnsMaxTtl = "network.dns.cache.max_ttl";
inline constexpr std::string_view DnsNegativeTtl = "network.dns.cache.negative_ttl";
inline constexpr std::string_view ProxyMode = "network.proxy.mode";
inline constexpr std::string_view ProxyType = "network.proxy.type";
inline constexpr std::string_view ProxyHost = "network.proxy.host";
inline constexpr std::string_view ProxyPort = "network.proxy.port";
inline constexpr std::string_view ProxyBypass = "network.proxy.bypass";
inline constexpr std::string_view ProxyBypassLocal = "network.proxy.bypass_local";
inline constexpr std::string_view HttpVersion = "network.connection.http_version";
inline constexpr std::string_view ConnectTimeout = "network.connection.connect_timeout";
inline constexpr std::string_view ReadTimeout = "network.connection.read_timeout";
inline constexpr std::string_view MaxConnections = "network.connection.max_connections";
inline constexpr std::string_view MaxConnectionsPerHost = "network.connection.max_per_host";
inline constexpr std::string_view Retries = "network.connection.retries";
inline constexpr std::string_view RetryBackoff = "network.connection.retry_backoff";
inline constexpr std::string_view TlsMinVersion = "network.tls.min_version";
inline constexpr std::string_view CaBundle = "network.tls.ca_bundle";

inline constexpr std::string_view AudioCacheEnabled = "cache.audio_enabled";
inline constexpr std::string_view AudioDiskMb = "cache.disk_mb";
inline constexpr std::string_view AudioRamMb = "cache.ram_mb";
inline constexpr std::string_view Prefetch = "cache.prefetch";
inline constexpr std::string_view EmbedTags = "cache.embed_tags";
inline constexpr std::string_view CacheDir = "cache.dir";
inline constexpr std::string_view ClearCacheOnExit = "cache.clear_on_exit";
inline constexpr std::string_view MinFreeMb = "cache.min_free_mb";
inline constexpr std::string_view ImageRamMb = "cache.images.ram_mb";
inline constexpr std::string_view ImageDiskMb = "cache.images.disk_mb";
inline constexpr std::string_view MetadataDiskMb = "cache.metadata.disk_mb";

inline constexpr std::string_view PageSize = "library.loading.page_size";

inline constexpr std::string_view ClientId = "soundcloud.client_id";

}  // namespace noctara::settings::keys
