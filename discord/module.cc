#define INSIDE_BLINK 1

#include "discord/discord_video_source.h"
#include "discord/script_compilation_timing.h"

#include "content/public/renderer/render_frame.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/node_includes.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_script_runner.h"  // nogncheck

#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-const-int-float-conversion"
#include "third_party/blink/renderer/modules/mediastream/media_stream.h"
#pragma GCC diagnostic pop
#include "third_party/blink/renderer/modules/mediastream/media_stream_constraints_util.h"
#include "third_party/blink/renderer/modules/mediastream/media_stream_video_track.h"
#include "third_party/blink/renderer/platform/bindings/v8_set_return_value.h"
#include "third_party/blink/renderer/platform/mediastream/media_stream_source.h"
#include "third_party/blink/renderer/platform/wtf/uuid.h"

namespace blink {

void DiscordStream(const v8::FunctionCallbackInfo<v8::Value>& info) {
  ExecutionContext* context =
      ExecutionContext::From(info.GetIsolate()->GetCurrentContext());

  auto* descriptor = MakeGarbageCollected<MediaStreamDescriptor>(
      CreateCanonicalUuidString(), MediaStreamComponentVector(),
      MediaStreamComponentVector());

  std::string streamId;
  gin::ConvertFromV8(info.GetIsolate(), info[0], &streamId);

  MediaStream* stream = MediaStream::Create(context, descriptor);
  std::unique_ptr<MediaStreamVideoSource> platform_source =
      std::make_unique<MediaStreamDiscordVideoSource>(streamId);
  auto* raw_platform_source = platform_source.get();
  const WebString track_id(CreateCanonicalUuidString());

  auto* media_stream_source = MakeGarbageCollected<MediaStreamSource>(
      track_id, MediaStreamSource::kTypeVideo, track_id, false,
      std::move(platform_source));
  MediaStreamSource::Capabilities capabilities;
  capabilities.device_id = track_id;
  media_stream_source->SetCapabilities(capabilities);
  descriptor->AddRemoteTrack(MediaStreamVideoTrack::CreateVideoTrack(
      raw_platform_source, MediaStreamVideoSource::ConstraintsOnceCallback(),
      true));

  bindings::V8SetReturnValue(info,
                             stream->ToV8(info.GetIsolate(), info.This()));
}
}  // namespace blink

namespace {

void Install(gin::Arguments* args, const std::string& name) {
  // Find our way to the main context from our isolate object
  v8::Local<v8::Context> context = args->isolate()->GetCurrentContext();
  blink::WebLocalFrame* frame = blink::WebLocalFrame::FrameForContext(context);
  content::RenderFrame* renderFrame = content::RenderFrame::FromWebFrame(frame);
  v8::Local<v8::Context> mainContext =
      renderFrame->GetWebFrame()->MainWorldScriptContext();
  // Get the global object (window) from the main context with a dictionary
  // wrapper
  gin_helper::Dictionary global(args->isolate(), mainContext->Global());

  v8::Local<v8::Value> wrappedFunction;
  // wrap our native function in a v8 object belonging to the main context
  if (!v8::Function::New(mainContext, blink::DiscordStream,
                         v8::Local<v8::Value>(), 1)
           .ToLocal(&wrappedFunction)) {
    args->isolate()->ThrowException(v8::Exception::Error(gin::StringToV8(
        args->isolate(), "Failed to allocate function object")));
    return;
  }
  global.SetReadOnlyNonConfigurable(name, wrappedFunction);
}

void StopScriptCompilationTimings(gin::Arguments* args) {
  blink::V8ScriptRunner::SetCompilationTimingCallback(nullptr);
}

v8::Local<v8::Value> GetScriptCompilationTimingsGin(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = isolate->GetCurrentContext();
  auto entries =
      discord::ScriptCompilationTimingCollector::GetInstance().GetEntries();

  v8::Local<v8::Array> result = v8::Array::New(isolate, entries.size());
  for (size_t i = 0; i < entries.size(); i++) {
    v8::Local<v8::Object> obj = v8::Object::New(isolate);
    obj->Set(context, gin::StringToV8(isolate, "url"),
             gin::StringToV8(isolate, entries[i].url))
        .Check();
    obj->Set(context, gin::StringToV8(isolate, "foregroundTimeUs"),
             v8::Number::New(
                 isolate, static_cast<double>(entries[i].foreground_time_us)))
        .Check();
    obj->Set(context, gin::StringToV8(isolate, "backgroundTimeUs"),
             v8::Number::New(
                 isolate, static_cast<double>(entries[i].background_time_us)))
        .Check();
    obj->Set(context, gin::StringToV8(isolate, "streamed"),
             v8::Boolean::New(isolate, entries[i].streamed))
        .Check();
    result->Set(context, i, obj).Check();
  }

  return result;
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  gin_helper::Dictionary dict(v8::Isolate::GetCurrent(), exports);
  dict.SetMethod("install", Install);
  dict.SetMethod("getScriptCompilationTimings", GetScriptCompilationTimingsGin);
  dict.SetMethod("stopScriptCompilationTimings", StopScriptCompilationTimings);
}
}  // namespace

NODE_LINKED_BINDING_CONTEXT_AWARE(electron_renderer_discord, Initialize)
