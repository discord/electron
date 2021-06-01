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

template <typename Base, typename... ExtraInterfaces>
class ElectronObject : public Base {
 public:
  virtual ElectronVideoStatus QueryInterface(char const* iid,
                                             void** ppUnknown) override {
    return RecursiveQueryInterface<IElectronUnknown, Base, ExtraInterfaces...>(
        iid, ppUnknown);
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

typedef void ElectronVideoSink(IElectronVideoFrame* decodedFrame,
                               void* userData);

class IElectronVideoDecoder : public IElectronUnknown {
 public:
  static constexpr char IID[] = "IElectronVideoDecoder";
  virtual ElectronVideoStatus Initialize(IElectronVideoFormat* format,
                                         ElectronVideoSink* videoSink,
                                         void* userData) = 0;
  virtual ElectronVideoStatus SubmitBuffer(IElectronBuffer* buffer,
                                           uint32_t timestamp) = 0;
};

extern "C" {
ELECTRON_VIDEO_EXPORT ElectronVideoStatus
ElectronVideoCreateObject(char const* clsid,
                          char const* iid,
                          void** ppElectronObject);
}

// This should be defined in exactly one source file to ensure the proper
// linkage for these constants exists (similar to COM INITGUID). This can go
// away once everybody is using C++17.
#ifdef ELECTRON_VIDEO_DECLARE_IIDS
constexpr char IElectronUnknown::IID[];
constexpr char IElectronBuffer::IID[];
constexpr char IElectronVideoFormat::IID[];
constexpr char IElectronVideoFrame::IID[];
constexpr char IElectronVideoDecoder::IID[];
#endif  // ELECTRON_VIDEO_DECLARE_IIDS

}  // namespace electron
}  // namespace media
}  // namespace discord
