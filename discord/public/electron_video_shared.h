#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

// This is a public header intended to be shared between the Electron codebase
// and Discord native modules. It shan't reference any Electron headers, nor
// Discord headers.

#ifdef ELECTRON_VIDEO_IMPLEMENTATION
#ifdef WIN32
#define ELECTRON_VIDEO_EXPORT __attribute__((dllexport))
#else
#define ELECTRON_VIDEO_EXPORT __attribute__((visibility("default")))
#endif  // WIN32
#else   // ELECTRON_VIDEO_IMPLEMENTATION
#define ELECTRON_VIDEO_EXPORT
#endif

namespace discord {
namespace media {
namespace electron {

#define ELECTRON_VIDEO_SUCCEEDED(val) \
  ((val) == ::discord::media::electron::ElectronVideoStatus::Success)
#define ELECTRON_VIDEO_RETURN_IF_ERR(expr)                        \
  do {                                                            \
    auto electron_video_status_tmp__ = (expr);                    \
                                                                  \
    if (!ELECTRON_VIDEO_SUCCEEDED(electron_video_status_tmp__)) { \
      return electron_video_status_tmp__;                         \
    }                                                             \
  } while (0)

enum class ElectronVideoStatus {
  Success = 0,
  Failure = 1,
  RuntimeLoadFailed = 2,
  InterfaceNotFound = 3,
  ClassNotFound = 4,
  Unsupported = 5,
  InvalidState = 6,
  TimedOut = 7,
};

class IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronUnknown";

  virtual ElectronVideoStatus QueryInterface(char const* iid,
                                             void** ppUnknown) = 0;
  virtual size_t AddRef() = 0;
  virtual size_t Release() = 0;
};

template <typename... Interfaces>
class ElectronObject : public Interfaces... {
 public:
  virtual ElectronVideoStatus QueryInterface(char const* iid,
                                             void** ppUnknown) override {
    if (!strcmp(iid, IElectronUnknown::IID)) {
      return QueryInterface(CanonicalBase<Interfaces...>::T::IID, ppUnknown);
    }
    return RecursiveQueryInterface<Interfaces...>(iid, ppUnknown);
  }
  virtual size_t AddRef() override { return ++refCount_; }
  virtual size_t Release() override {
    auto newCount = --refCount_;

    if (newCount == 0) {
      delete this;
    }

    return newCount;
  }

 protected:
  ElectronObject() {}
  virtual ~ElectronObject() {}

 private:
  template <typename Head, typename... Tail>
  struct CanonicalBase {
    using T = Head;
  };
  ElectronObject(ElectronObject const&) = delete;
  ElectronObject& operator=(ElectronObject const&) = delete;

  template <typename... Tail>
  inline std::enable_if_t<sizeof...(Tail) == 0, ElectronVideoStatus>
  RecursiveQueryInterface(char const* iid, void** ppUnknown) {
    *ppUnknown = nullptr;
    return ElectronVideoStatus::InterfaceNotFound;
  }
  template <typename Head, typename... Tail>
  inline ElectronVideoStatus RecursiveQueryInterface(char const* iid,
                                                     void** ppUnknown) {
    if (!strcmp(iid, Head::IID)) {
      *ppUnknown = static_cast<Head*>(this);
      this->AddRef();
      return ElectronVideoStatus::Success;
    }

    return RecursiveQueryInterface<Tail...>(iid, ppUnknown);
  }
  std::atomic<size_t> refCount_{1};
};

template <typename T>
class ElectronPointer final {
 public:
  static_assert(std::is_base_of<IElectronUnknown, T>::value,
                "Electron Video objects must descend from IElectronUnknown");

  ElectronPointer(T* electronObject = nullptr)
      : electronObject_(electronObject) {}

  ElectronPointer(ElectronPointer&& rhs)
      : electronObject_(rhs.electronObject_) {
    rhs.electronObject_ = nullptr;
  }

  ~ElectronPointer() { Release(); }

  ElectronPointer& operator=(ElectronPointer&& rhs) {
    Release();
    electronObject_ = rhs.electronObject_;
    rhs.electronObject_ = nullptr;
    return *this;
  }

  operator T*() const { return electronObject_; }
  T& operator*() { return *electronObject_; }
  T* operator->() { return electronObject_; }

  T** Receive() { return &electronObject_; }

  template <typename S>
  typename std::enable_if<std::is_void<S>::value, void**>::type Receive() {
    return reinterpret_cast<void**>(&electronObject_);
  }

  T* Abandon() {
    auto old = electronObject_;
    electronObject_ = nullptr;
    return old;
  }

 private:
  ElectronPointer(ElectronPointer const&) = delete;
  ElectronPointer& operator=(ElectronPointer const&) = delete;

  void Release() {
    if (electronObject_) {
      auto temp = electronObject_;
      electronObject_ = nullptr;
      temp->Release();
    }
  }

  T* electronObject_;
};

template <typename T>
ElectronPointer<T> RetainElectronVideoObject(T* ptr) {
  if (ptr) {
    ptr->AddRef();
  }

  return ptr;
}

class IElectronBuffer : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronBuffer";
  virtual uint8_t* GetBytes() = 0;
  virtual size_t GetLength() = 0;
};

// These enums are all stolen from media/ and must be kept in sync.

// GENERATED_JAVA_ENUM_PACKAGE: org.chromium.media
enum ElectronVideoCodec {
  // These values are histogrammed over time; do not change their ordinal
  // values.  When deleting a codec replace it with a dummy value; when adding a
  // codec, do so at the bottom (and update kVideoCodecMax).
  kUnknownVideoCodec = 0,
  kCodecH264,
  kCodecVC1,
  kCodecMPEG2,
  kCodecMPEG4,
  kCodecTheora,
  kCodecVP8,
  kCodecVP9,
  kCodecHEVC,
  kCodecDolbyVision,
  kCodecAV1,
  // DO NOT ADD RANDOM VIDEO CODECS!
  //
  // The only acceptable time to add a new codec is if there is production code
  // that uses said codec in the same CL.

  kVideoCodecMax = kCodecAV1,  // Must equal the last "real" codec above.
};

// Video codec profiles. Keep in sync with mojo::VideoCodecProfile (see
// media/mojo/mojom/media_types.mojom), gpu::VideoCodecProfile (see
// gpu/config/gpu_info.h), and PP_VideoDecoder_Profile (translation is performed
// in content/renderer/pepper/ppb_video_decoder_impl.cc).
// NOTE: These values are histogrammed over time in UMA so the values must never
// ever change (add new values to tools/metrics/histograms/histograms.xml)
// GENERATED_JAVA_ENUM_PACKAGE: org.chromium.media
enum ElectronVideoCodecProfile {
  // Keep the values in this enum unique, as they imply format (h.264 vs. VP8,
  // for example), and keep the values for a particular format grouped
  // together for clarity.
  VIDEO_CODEC_PROFILE_UNKNOWN = -1,
  VIDEO_CODEC_PROFILE_MIN = VIDEO_CODEC_PROFILE_UNKNOWN,
  H264PROFILE_MIN = 0,
  H264PROFILE_BASELINE = H264PROFILE_MIN,
  H264PROFILE_MAIN = 1,
  H264PROFILE_EXTENDED = 2,
  H264PROFILE_HIGH = 3,
  H264PROFILE_HIGH10PROFILE = 4,
  H264PROFILE_HIGH422PROFILE = 5,
  H264PROFILE_HIGH444PREDICTIVEPROFILE = 6,
  H264PROFILE_SCALABLEBASELINE = 7,
  H264PROFILE_SCALABLEHIGH = 8,
  H264PROFILE_STEREOHIGH = 9,
  H264PROFILE_MULTIVIEWHIGH = 10,
  H264PROFILE_MAX = H264PROFILE_MULTIVIEWHIGH,
  VP8PROFILE_MIN = 11,
  VP8PROFILE_ANY = VP8PROFILE_MIN,
  VP8PROFILE_MAX = VP8PROFILE_ANY,
  VP9PROFILE_MIN = 12,
  VP9PROFILE_PROFILE0 = VP9PROFILE_MIN,
  VP9PROFILE_PROFILE1 = 13,
  VP9PROFILE_PROFILE2 = 14,
  VP9PROFILE_PROFILE3 = 15,
  VP9PROFILE_MAX = VP9PROFILE_PROFILE3,
  HEVCPROFILE_MIN = 16,
  HEVCPROFILE_MAIN = HEVCPROFILE_MIN,
  HEVCPROFILE_MAIN10 = 17,
  HEVCPROFILE_MAIN_STILL_PICTURE = 18,
  HEVCPROFILE_MAX = HEVCPROFILE_MAIN_STILL_PICTURE,
  DOLBYVISION_PROFILE0 = 19,
  DOLBYVISION_PROFILE4 = 20,
  DOLBYVISION_PROFILE5 = 21,
  DOLBYVISION_PROFILE7 = 22,
  THEORAPROFILE_MIN = 23,
  THEORAPROFILE_ANY = THEORAPROFILE_MIN,
  THEORAPROFILE_MAX = THEORAPROFILE_ANY,
  AV1PROFILE_MIN = 24,
  AV1PROFILE_PROFILE_MAIN = AV1PROFILE_MIN,
  AV1PROFILE_PROFILE_HIGH = 25,
  AV1PROFILE_PROFILE_PRO = 26,
  AV1PROFILE_MAX = AV1PROFILE_PROFILE_PRO,
  DOLBYVISION_PROFILE8 = 27,
  DOLBYVISION_PROFILE9 = 28,
  VIDEO_CODEC_PROFILE_MAX = DOLBYVISION_PROFILE9,
};

// Synced with media/base/video_types.h
enum ElectronVideoPixelFormat {
  PIXEL_FORMAT_UNKNOWN = 0,  // Unknown or unspecified format value.
  PIXEL_FORMAT_I420 =
      1,  // 12bpp YUV planar 1x1 Y, 2x2 UV samples, a.k.a. YU12.

  // Note: Chrome does not actually support YVU compositing, so you probably
  // don't actually want to use this. See http://crbug.com/784627.
  PIXEL_FORMAT_YV12 = 2,  // 12bpp YVU planar 1x1 Y, 2x2 VU samples.

  PIXEL_FORMAT_I422 = 3,   // 16bpp YUV planar 1x1 Y, 2x1 UV samples.
  PIXEL_FORMAT_I420A = 4,  // 20bpp YUVA planar 1x1 Y, 2x2 UV, 1x1 A samples.
  PIXEL_FORMAT_I444 = 5,   // 24bpp YUV planar, no subsampling.
  PIXEL_FORMAT_NV12 =
      6,  // 12bpp with Y plane followed by a 2x2 interleaved UV plane.
  PIXEL_FORMAT_NV21 =
      7,  // 12bpp with Y plane followed by a 2x2 interleaved VU plane.
  PIXEL_FORMAT_UYVY =
      8,  // 16bpp interleaved 2x1 U, 1x1 Y, 2x1 V, 1x1 Y samples.
  PIXEL_FORMAT_YUY2 =
      9,  // 16bpp interleaved 1x1 Y, 2x1 U, 1x1 Y, 2x1 V samples.
  PIXEL_FORMAT_ARGB = 10,   // 32bpp BGRA (byte-order), 1 plane.
  PIXEL_FORMAT_XRGB = 11,   // 24bpp BGRX (byte-order), 1 plane.
  PIXEL_FORMAT_RGB24 = 12,  // 24bpp BGR (byte-order), 1 plane.

  /* PIXEL_FORMAT_RGB32 = 13,  Deprecated */
  PIXEL_FORMAT_MJPEG = 14,  // MJPEG compressed.
  /* PIXEL_FORMAT_MT21 = 15,  Deprecated */

  // The P* in the formats below designates the number of bits per pixel
  // component. I.e. P9 is 9-bits per pixel component, P10 is 10-bits per pixel
  // component, etc.
  PIXEL_FORMAT_YUV420P9 = 16,
  PIXEL_FORMAT_YUV420P10 = 17,
  PIXEL_FORMAT_YUV422P9 = 18,
  PIXEL_FORMAT_YUV422P10 = 19,
  PIXEL_FORMAT_YUV444P9 = 20,
  PIXEL_FORMAT_YUV444P10 = 21,
  PIXEL_FORMAT_YUV420P12 = 22,
  PIXEL_FORMAT_YUV422P12 = 23,
  PIXEL_FORMAT_YUV444P12 = 24,

  /* PIXEL_FORMAT_Y8 = 25, Deprecated */
  PIXEL_FORMAT_Y16 = 26,  // single 16bpp plane.

  PIXEL_FORMAT_ABGR = 27,  // 32bpp RGBA (byte-order), 1 plane.
  PIXEL_FORMAT_XBGR = 28,  // 24bpp RGBX (byte-order), 1 plane.

  PIXEL_FORMAT_P016LE = 29,  // 24bpp NV12, 16 bits per channel

  PIXEL_FORMAT_XR30 =
      30,  // 32bpp BGRX, 10 bits per channel, 2 bits ignored, 1 plane
  PIXEL_FORMAT_XB30 =
      31,  // 32bpp RGBX, 10 bits per channel, 2 bits ignored, 1 plane

  PIXEL_FORMAT_BGRA = 32,  // 32bpp ARGB (byte-order), 1 plane.

  PIXEL_FORMAT_RGBAF16 = 33,  // Half float RGBA, 1 plane.

  // Please update UMA histogram enumeration when adding new formats here.
  PIXEL_FORMAT_MAX =
      PIXEL_FORMAT_RGBAF16,  // Must always be equal to largest entry logged.
};

// Synced with media/base/video_frame.h
enum ElectronVideoStorageType {
  STORAGE_UNKNOWN = 0,
  STORAGE_OPAQUE = 1,  // We don't know how VideoFrame's pixels are stored.
  STORAGE_UNOWNED_MEMORY = 2,  // External, non owned data pointers.
  STORAGE_OWNED_MEMORY = 3,    // VideoFrame has allocated its own data buffer.
  STORAGE_SHMEM = 4,           // Backed by unsafe (writable) shared memory.
  STORAGE_DMABUFS = 5,         // Each plane is stored into a DmaBuf.
  // Backed by a mojo shared buffer. This should only be used by the
  // MojoSharedBufferVideoFrame subclass.
  STORAGE_MOJO_SHARED_BUFFER = 6,
  STORAGE_GPU_MEMORY_BUFFER = 7,
  STORAGE_LAST = STORAGE_GPU_MEMORY_BUFFER,
};

class IElectronVideoFormat : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoFormat";
  virtual ElectronVideoStatus SetCodec(ElectronVideoCodec codec) = 0;
  virtual ElectronVideoCodec GetCodec() = 0;
  virtual ElectronVideoStatus SetProfile(ElectronVideoCodecProfile profile) = 0;
  virtual ElectronVideoCodecProfile GetProfile() = 0;
};

class IElectronVideoFrame : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoFrame";
  virtual uint32_t GetWidth() = 0;
  virtual uint32_t GetHeight() = 0;
  virtual uint32_t GetTimestamp() = 0;
  virtual ElectronVideoStatus ToI420(IElectronBuffer* outputBuffer) = 0;
};

class IElectronVideoFrameData : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoData";
  virtual bool IsMappable() = 0;
  virtual bool HasTextures() = 0;
  virtual bool HasGpuMemoryBuffer() = 0;
  virtual ElectronVideoPixelFormat GetFormat() = 0;
  virtual ElectronVideoStorageType GetStorageType() = 0;
  virtual int GetStride(size_t plane) = 0;
  virtual int GetRowBytes(size_t plane) = 0;
  virtual int GetRows(size_t plane) = 0;
  virtual uint8_t const* GetData(size_t plane) = 0;
};

// TODO(eiz): deprecated name.
typedef void ElectronVideoSink(IElectronVideoFrame* decodedFrame,
                               void* userData);
typedef ElectronVideoSink ElectronVideoDecodedCB;

class IElectronVideoDecoder : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoDecoder";
  virtual ElectronVideoStatus Initialize(IElectronVideoFormat* format,
                                         ElectronVideoDecodedCB* callback,
                                         void* userData) = 0;
  virtual ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                           uint32_t timestamp) = 0;
};

class IElectronVideoSinkAttachment : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoSinkAttachment";
  virtual ElectronVideoStatus OnFrame(IElectronVideoFrame* frame,
                                      int64_t estimated_capture_timestamp) = 0;
};

class IElectronVideoSinkManager : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoSinkManager";
  virtual ElectronVideoStatus SetAttachment(
      char const* sinkId,
      IElectronVideoSinkAttachment* attachment) = 0;
};

class IElectronVideoDebugLoggable : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoDebugLoggable";
  virtual void PrintDebugLog() = 0;
};

extern "C" {
ELECTRON_VIDEO_EXPORT ElectronVideoStatus
ElectronVideoCreateObject(char const* clsid,
                          char const* iid,
                          void** ppElectronObject);
}

struct DiscordYUVFrame {
  const uint8_t* y;
  const uint8_t* u;
  const uint8_t* v;
  int32_t y_stride;
  int32_t u_stride;
  int32_t v_stride;
};

enum Rotation : int32_t { kRotation0, kRotation90, kRotation180, kRotation270 };

struct DiscordFrame {
  int64_t timestamp_us;
  union {
    DiscordYUVFrame yuv;
#if defined(_WIN32)
    void* texture_handle;
#endif
    IElectronVideoFrame* electron;
  } frame;
  int32_t width;
  int32_t height;
  int32_t type;
  Rotation rotation;
};

// This should be defined in exactly one source file to ensure the proper
// linkage for these constants exists (similar to COM INITGUID). This can go
// away once everybody is using C++17.
#ifdef ELECTRON_VIDEO_DECLARE_IIDS
constexpr char IElectronUnknown::IID[];
constexpr char IElectronBuffer::IID[];
constexpr char IElectronVideoFormat::IID[];
constexpr char IElectronVideoFrame::IID[];
constexpr char IElectronVideoFrameData::IID[];
constexpr char IElectronVideoDecoder::IID[];
constexpr char IElectronVideoSinkAttachment::IID[];
constexpr char IElectronVideoSinkManager::IID[];
constexpr char IElectronVideoDebugLoggable::IID[];
#endif  // ELECTRON_VIDEO_DECLARE_IIDS

}  // namespace electron
}  // namespace media
}  // namespace discord
