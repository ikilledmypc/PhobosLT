// Keeps the phone's screen on during a race, so the page keeps receiving lap events.
// Uses the Screen Wake Lock API where available (HTTPS/localhost only); otherwise plays
// a tiny muted looping video, which browsers also treat as a reason to keep the screen on.
// The setting is stored per device in localStorage, not in the timer's config.

const keepAwakeInput = document.getElementById("keepAwake");
const keepAwakeStorageKey = "keepAwake";
// 16x16 black, 2 second, silent H.264/AAC clip.
const keepAwakeVideoSrc = "data:video/mp4;base64,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";

let keepAwakeActive = false;
let keepAwakeLock = null;
let keepAwakeVideo = null;

function isKeepAwakeEnabled() {
  try {
    return localStorage.getItem(keepAwakeStorageKey) !== "0";  // on by default
  } catch (e) {
    return true;
  }
}

function acquireKeepAwake() {
  if ("wakeLock" in navigator && window.isSecureContext) {
    navigator.wakeLock
      .request("screen")
      .then((lock) => (keepAwakeLock = lock))
      .catch((e) => console.log("Wake lock failed: " + e));
    return;
  }

  if (!keepAwakeVideo) {
    keepAwakeVideo = document.createElement("video");
    keepAwakeVideo.setAttribute("playsinline", "");
    keepAwakeVideo.setAttribute("muted", "");
    keepAwakeVideo.muted = true;
    keepAwakeVideo.loop = true;
    keepAwakeVideo.src = keepAwakeVideoSrc;
    // Kept (barely) on screen, some browsers don't keep the screen on for offscreen videos.
    keepAwakeVideo.style.cssText =
      "position:fixed;right:0;bottom:0;width:1px;height:1px;opacity:0.01;pointer-events:none";
    document.body.appendChild(keepAwakeVideo);
  }
  keepAwakeVideo.play().catch((e) => console.log("Keep awake video failed: " + e));
}

function releaseKeepAwake() {
  if (keepAwakeLock) {
    keepAwakeLock.release();
    keepAwakeLock = null;
  }
  if (keepAwakeVideo) {
    keepAwakeVideo.pause();
  }
}

// Must be called from a user action (e.g. the Start Race click), browsers only allow
// starting playback from one.
function keepAwakeStart() {
  keepAwakeActive = isKeepAwakeEnabled();
  if (keepAwakeActive) {
    acquireKeepAwake();
  }
}

function keepAwakeStop() {
  keepAwakeActive = false;
  releaseKeepAwake();
}

// Both the wake lock and the video are stopped when the page is hidden, resume when it's back.
document.addEventListener("visibilitychange", () => {
  if (keepAwakeActive && document.visibilityState === "visible") {
    acquireKeepAwake();
  }
});

keepAwakeInput.checked = isKeepAwakeEnabled();
keepAwakeInput.addEventListener("change", () => {
  try {
    localStorage.setItem(keepAwakeStorageKey, keepAwakeInput.checked ? "1" : "0");
  } catch (e) {}
  if (!keepAwakeInput.checked) {
    keepAwakeStop();
  }
});
