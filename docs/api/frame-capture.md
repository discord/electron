## Class: FrameCapture

> Streams the rendered output of a single frame to native code in another renderer.

Process: [Main](../glossary.md#main-process)<br />
_This class is not exported from the `'electron'` module. It is only available as a return value of other methods in the Electron API._

Created with [`frame.captureStream(options)`](web-frame-main.md#framecapturestreamoptions).
Frames are delivered as platform shared textures (a DXGI NT handle on Windows,
an IOSurface on macOS and dmabuf planes on Linux) to the consumer frame's
renderer, where native code registers a sink for the capture id. On Linux,
shared memory is used when the GPU cannot export native buffers.

Frames go from the GPU process and audio from the audio service straight to
the consumer renderer, off its main thread. Neither passes through the main
process, so a busy main process doesn't delay delivery; it only controls the
capture and emits the events below.

```js
const { webContents } = require('electron')

const contents = webContents.getFocusedWebContents()
const activity = contents.mainFrame.framesInSubtree.find(
  (frame) => frame.origin.endsWith('.discordsays.com'))

const capture = activity.captureStream({
  consumerFrame: contents.mainFrame,
  audio: true
})

capture.on('stopped', (event, reason) => {
  console.log('Capture stopped:', reason)
})
```

### Instance Events

#### Event: 'started'

Returns:

* `event` Event
* `details` Object
  * `captureId` string
  * `targetKind` string - Can be `main-frame`, `oopif` or `subtree`. `subtree`
    is a same-process iframe, captured as its element in the parent's
    output. While it is captured, the iframe element computes
    `isolation: isolate`.
  * `adapterLuid` Object (optional) _Windows_ - The adapter the textures were
    allocated on.
    * `low` Integer
    * `high` Integer

Emitted once the capturer is attached to the frame's compositor output.

#### Event: 'resized'

Returns:

* `event` Event
* `size` [Size](structures/size.md)

Emitted when the size of the captured frames changes.

#### Event: 'paused'

Emitted when the captured frame stops being rendered in the viewport, for
example when its element is hidden or scrolled out of view. No new frames
are produced until it is shown again.

#### Event: 'resumed'

Emitted when a paused frame is rendered in the viewport again.

#### Event: 'stopped'

Returns:

* `event` Event
* `reason` string - Can be `stopped`, `frame-destroyed`, `consumer-gone`,
  `consumer-unavailable` or `target-unsupported`.

Emitted when the capture ends. The session cannot be restarted. The native
sink receives no frames or audio after its `OnCaptureStopped`.

### Instance Methods

#### `capture.stop()`

Stops the capture.

#### `capture.setConstraints(constraints)`

* `constraints` Object
  * `maxFrameRate` Integer (optional)
  * `maxSize` [Size](structures/size.md) (optional) - An empty size removes the
    limit.
  * `audio` boolean (optional) - Starts or stops capturing the frame's audio.

Changes the capture constraints while the capture is running.

### Instance Properties

#### `capture.captureId` _Readonly_

A `string` identifying this capture to the consumer.

#### `capture.capturing` _Readonly_

A `boolean` indicating whether the capture is still running.
