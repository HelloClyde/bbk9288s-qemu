import RFB from "@novnc/novnc";
import {
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  ChevronUp,
  CircleCheck,
  CornerUpLeft,
  Download,
  File,
  Folder,
  FolderOpen,
  FolderPlus,
  HardDrive,
  Maximize2,
  Monitor,
  Pencil,
  Power,
  RefreshCw,
  Save,
  Trash2,
  Undo2,
  Upload,
  Volume2,
  VolumeX,
  X,
  createIcons,
} from "lucide";
import "./styles.css";

const iconSet = {
  ChevronDown,
  ChevronLeft,
  ChevronRight,
  ChevronUp,
  CircleCheck,
  CornerUpLeft,
  Download,
  File,
  Folder,
  FolderOpen,
  FolderPlus,
  HardDrive,
  Maximize2,
  Monitor,
  Pencil,
  Power,
  RefreshCw,
  Save,
  Trash2,
  Undo2,
  Upload,
  Volume2,
  VolumeX,
  X,
};

function renderIcons() {
  createIcons({ icons: iconSet });
}

renderIcons();

const screen = document.querySelector("#screen");
const screenState = document.querySelector("#screen-state");
const statusText = document.querySelector("#status-text");
const statusNode = document.querySelector(".connection-state");
const powerButton = document.querySelector("#device-power");
const reconnectButton = document.querySelector("#reconnect");
const fullscreenButton = document.querySelector("#fullscreen");
const deviceFrame = document.querySelector("#device-frame");
const touchLayer = document.querySelector("#touch-layer");
const audioElement = document.querySelector("#audio-stream");
const audioToggle = document.querySelector("#audio-toggle");
const audioVolume = document.querySelector("#audio-volume");
const audioVolumeValue = document.querySelector("#audio-volume-value");

const keyMap = {
  up: { keysym: 0xff52, code: "ArrowUp" },
  down: { keysym: 0xff54, code: "ArrowDown" },
  left: { keysym: 0xff51, code: "ArrowLeft" },
  right: { keysym: 0xff53, code: "ArrowRight" },
  enter: { keysym: 0xff0d, code: "Enter" },
  escape: { keysym: 0xff1b, code: "Escape" },
};
const minimumKeyHoldMs = 40;
const minimumTouchHoldMs = 180;

let rfb = null;
let reconnectTimer = null;
let manualDisconnect = false;
let powerBusy = false;
let audioEnabled = false;
let audioReconnectTimer = null;

const savedVolume = Number.parseFloat(
  window.localStorage.getItem("bbk9288s.audioVolume") || "0.8",
);
audioElement.volume = Number.isFinite(savedVolume)
  ? Math.min(1, Math.max(0, savedVolume))
  : 0.8;
audioVolume.value = String(audioElement.volume);

function audioStreamUrl(offset) {
  return `/api/audio/stream?offset=${offset}&session=${Date.now()}`;
}

function setAudioState() {
  const muted = audioElement.muted || audioElement.volume === 0;
  audioToggle.dataset.enabled = String(audioEnabled);
  audioToggle.dataset.muted = String(muted);
  audioToggle.title = !audioEnabled
    ? "启用声音"
    : muted
      ? "取消静音"
      : "静音";
  audioToggle.setAttribute("aria-label", audioToggle.title);
  audioVolumeValue.value = `${Math.round(audioElement.volume * 100)}%`;
}

async function openAudioStream() {
  if (!audioEnabled) {
    return;
  }
  window.clearTimeout(audioReconnectTimer);
  try {
    const status = await apiRequest("/api/status");
    if (!audioEnabled) {
      return;
    }
    const offset = Math.max(0, Number(status.audioBytes) || 0);
    audioElement.pause();
    audioElement.src = audioStreamUrl(offset);
    audioElement.load();
    const playing = audioElement.play();
    if (playing) {
      playing.catch((error) => {
        if (error.name !== "AbortError") {
          console.error("audio playback failed", error);
        }
      });
    }
  } catch (error) {
    console.error("audio stream connection failed", error);
    scheduleAudioReconnect();
  }
}

function scheduleAudioReconnect() {
  window.clearTimeout(audioReconnectTimer);
  if (!audioEnabled) {
    return;
  }
  audioReconnectTimer = window.setTimeout(openAudioStream, 1200);
}

audioToggle.addEventListener("click", () => {
  if (!audioEnabled) {
    audioEnabled = true;
    audioElement.muted = false;
    openAudioStream();
  } else {
    audioElement.muted = !audioElement.muted;
    if (!audioElement.muted && audioElement.paused) {
      openAudioStream();
    }
  }
  setAudioState();
});

audioVolume.addEventListener("input", () => {
  audioElement.volume = Number(audioVolume.value);
  audioElement.muted = audioElement.volume === 0;
  window.localStorage.setItem(
    "bbk9288s.audioVolume",
    String(audioElement.volume),
  );
  if (!audioEnabled && audioElement.volume > 0) {
    audioEnabled = true;
    openAudioStream();
  }
  setAudioState();
});

audioElement.addEventListener("ended", scheduleAudioReconnect);
audioElement.addEventListener("error", scheduleAudioReconnect);
setAudioState();

function websocketUrl() {
  const params = new URLSearchParams(window.location.search);
  const explicit = params.get("ws");
  if (explicit) {
    return explicit;
  }

  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const host = params.get("host") || window.location.hostname;
  const port = params.get("wsPort") || "6081";
  return `${protocol}//${host}:${port}`;
}

function setConnectionState(state, text) {
  statusNode.dataset.state = state;
  statusText.textContent = text;
  screenState.textContent = text;
  screenState.hidden = state === "connected";
}

function setPowerState(status) {
  const maintenance = Boolean(status?.maintenance);
  const running = Boolean(status?.emulatorRunning);
  const label = maintenance
    ? "NAND 维护中"
    : running
      ? "重启设备"
      : "启动设备";

  powerButton.title = powerBusy ? "正在启动设备" : label;
  powerButton.setAttribute("aria-label", powerButton.title);
  powerButton.disabled = powerBusy || maintenance;
  powerButton.classList.toggle("is-busy", powerBusy);
  powerButton.dataset.running = String(running);
}

function scheduleReconnect() {
  window.clearTimeout(reconnectTimer);
  reconnectTimer = window.setTimeout(connect, 1500);
}

function connect() {
  window.clearTimeout(reconnectTimer);
  manualDisconnect = false;
  if (rfb) {
    rfb.disconnect();
    rfb = null;
  }

  setConnectionState("connecting", "正在连接");
  try {
    rfb = new RFB(screen, websocketUrl(), { shared: true });
    rfb.scaleViewport = true;
    rfb.resizeSession = false;
    rfb.clipViewport = false;
    rfb.dragViewport = false;
    rfb.focusOnClick = true;
    rfb.showDotCursor = false;
    rfb.background = "#f7f7f4";

    rfb.addEventListener("connect", () => {
      setConnectionState("connected", "已连接");
      screen.focus({ preventScroll: true });
    });
    rfb.addEventListener("disconnect", (event) => {
      rfb = null;
      if (manualDisconnect) {
        setConnectionState("offline", "已断开");
        return;
      }
      setConnectionState("offline", event.detail.clean ? "已断开" : "连接中断");
      scheduleReconnect();
    });
    rfb.addEventListener("securityfailure", () => {
      setConnectionState("error", "连接被拒绝");
    });
    rfb.addEventListener("credentialsrequired", () => {
      setConnectionState("error", "需要凭据");
    });
  } catch (error) {
    console.error(error);
    rfb = null;
    setConnectionState("error", "无法连接");
    scheduleReconnect();
  }
}

function sendKey(name, down) {
  const key = keyMap[name];
  if (!rfb || !key) {
    return;
  }
  rfb.sendKey(key.keysym, key.code, down);
}

function pointerPosition(event) {
  const canvas = screen.querySelector("canvas");
  if (!canvas) {
    return null;
  }

  const bounds = canvas.getBoundingClientRect();
  if (
    event.clientX < bounds.left ||
    event.clientX >= bounds.right ||
    event.clientY < bounds.top ||
    event.clientY >= bounds.bottom
  ) {
    return null;
  }

  return {
    x: event.clientX - bounds.left,
    y: event.clientY - bounds.top,
  };
}

let activeTouchPointer = null;
let touchPressedAt = 0;
let touchPosition = null;
let touchReleaseTimer = null;

function sendPointerButton(position, mask) {
  // noVNC has no public pointer injection API. This keeps input on its normal
  // RFB path while allowing the original firmware's touch debounce to settle.
  if (!rfb || !position || typeof rfb._handleMouseButton !== "function") {
    return;
  }
  rfb._handleMouseButton(position.x, position.y, mask);
}

function releaseTouch(event) {
  if (activeTouchPointer === null || touchReleaseTimer !== null) {
    return;
  }

  if (event) {
    touchPosition = pointerPosition(event) || touchPosition;
  }
  const finishRelease = () => {
    sendPointerButton(touchPosition, 0);
    activeTouchPointer = null;
    touchPosition = null;
    touchReleaseTimer = null;
  };
  const heldFor = window.performance.now() - touchPressedAt;
  touchReleaseTimer = window.setTimeout(
    finishRelease,
    Math.max(0, minimumTouchHoldMs - heldFor),
  );
  event?.preventDefault();
}

touchLayer.addEventListener("pointerdown", (event) => {
  const position = pointerPosition(event);
  if (activeTouchPointer !== null || !position) {
    return;
  }

  activeTouchPointer = event.pointerId;
  touchPressedAt = window.performance.now();
  touchPosition = position;
  touchLayer.setPointerCapture(event.pointerId);
  screen.focus({ preventScroll: true });
  sendPointerButton(position, 1);
  event.preventDefault();
});

touchLayer.addEventListener("pointermove", (event) => {
  if (event.pointerId !== activeTouchPointer || !rfb) {
    return;
  }

  const position = pointerPosition(event);
  if (!position) {
    return;
  }
  touchPosition = position;
  if (typeof rfb._handleMouseMove === "function") {
    rfb._handleMouseMove(position.x, position.y);
  }
  event.preventDefault();
});

touchLayer.addEventListener("pointerup", releaseTouch);
touchLayer.addEventListener("pointercancel", releaseTouch);
touchLayer.addEventListener("lostpointercapture", releaseTouch);

for (const button of document.querySelectorAll("[data-key]")) {
  let activePointer = null;
  let pressedAt = 0;
  let releaseTimer = null;

  const release = (event) => {
    if (activePointer === null || releaseTimer !== null) {
      return;
    }
    const finishRelease = () => {
      sendKey(button.dataset.key, false);
      button.classList.remove("is-pressed");
      activePointer = null;
      releaseTimer = null;
    };
    const heldFor = window.performance.now() - pressedAt;
    const remaining = Math.max(0, minimumKeyHoldMs - heldFor);
    releaseTimer = window.setTimeout(finishRelease, remaining);
    event?.preventDefault();
  };

  button.addEventListener("pointerdown", (event) => {
    if (activePointer !== null) {
      return;
    }
    activePointer = event.pointerId;
    pressedAt = window.performance.now();
    button.setPointerCapture(event.pointerId);
    button.classList.add("is-pressed");
    sendKey(button.dataset.key, true);
    event.preventDefault();
  });
  button.addEventListener("pointerup", release);
  button.addEventListener("pointercancel", release);
  button.addEventListener("lostpointercapture", release);
}

reconnectButton.addEventListener("click", () => {
  manualDisconnect = true;
  if (rfb) {
    rfb.disconnect();
    rfb = null;
  }
  connect();
});

powerButton.addEventListener("click", async () => {
  if (powerBusy) {
    return;
  }

  const wasRunning = Boolean(managerStatus?.emulatorRunning);
  powerBusy = true;
  setPowerState();
  window.clearTimeout(reconnectTimer);
  manualDisconnect = true;
  rfb?.disconnect();
  rfb = null;
  setConnectionState("connecting", "正在启动");

  try {
    const status = await apiRequest("/api/emulator/restart", {
      method: "POST",
    });
    managerStatus = status;
    setPowerState(status);
    showToast(wasRunning ? "设备已重启" : "设备已启动");
    connect();
    openAudioStream();
  } catch (error) {
    console.error(error);
    setConnectionState("error", "启动失败");
    showToast(error.message || "设备启动失败", true);
  } finally {
    powerBusy = false;
    setPowerState(managerStatus);
  }
});

fullscreenButton.addEventListener("click", async () => {
  if (document.fullscreenElement) {
    await document.exitFullscreen();
  } else {
    await deviceFrame.requestFullscreen();
  }
});

document.addEventListener("fullscreenchange", () => {
  fullscreenButton.title = document.fullscreenElement ? "退出全屏" : "全屏";
  fullscreenButton.setAttribute("aria-label", fullscreenButton.title);
});

window.addEventListener("beforeunload", () => {
  manualDisconnect = true;
  rfb?.disconnect();
  window.clearTimeout(audioReconnectTimer);
  audioElement.pause();
});

connect();

const viewTabs = document.querySelectorAll("[data-view]");
const viewPanels = document.querySelectorAll("[data-view-panel]");
const filesView = document.querySelector("#files-view");
const maintenanceGate = document.querySelector("#maintenance-gate");
const fileBrowser = document.querySelector("#file-browser");
const fileActions = document.querySelector("#file-actions");
const fileList = document.querySelector("#file-list");
const emptyState = document.querySelector("#empty-state");
const breadcrumbs = document.querySelector("#breadcrumbs");
const operationState = document.querySelector("#operation-state");
const operationText = document.querySelector("#operation-text");
const capacityUsed = document.querySelector("#capacity-used");
const capacityFree = document.querySelector("#capacity-free");
const capacityProgress = document.querySelector("#capacity-progress");
const changeState = document.querySelector("#change-state");
const fileInput = document.querySelector("#file-input");
const nameDialog = document.querySelector("#name-dialog");
const nameDialogTitle = document.querySelector("#name-dialog-title");
const nameInput = document.querySelector("#name-input");
const nameSubmit = document.querySelector("#name-submit");
const confirmDialog = document.querySelector("#confirm-dialog");
const confirmTitle = document.querySelector("#confirm-title");
const confirmMessage = document.querySelector("#confirm-message");
const toast = document.querySelector("#toast");
const gateTitle = maintenanceGate.querySelector("h2");

let managerBusy = false;
let managerStatus = null;
let currentPath = "/";
let currentEntries = [];
let toastTimer = null;

async function apiRequest(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (
    options.body &&
    typeof options.body === "string" &&
    !headers.has("Content-Type")
  ) {
    headers.set("Content-Type", "application/json");
  }
  const response = await fetch(path, { ...options, headers });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(payload.error || `请求失败 (${response.status})`);
  }
  return payload;
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value <= 0) {
    return "0 B";
  }
  const units = ["B", "KiB", "MiB", "GiB"];
  const index = Math.min(
    Math.floor(Math.log(value) / Math.log(1024)),
    units.length - 1,
  );
  const amount = value / 1024 ** index;
  return `${amount >= 10 || index === 0 ? amount.toFixed(0) : amount.toFixed(1)} ${units[index]}`;
}

function formatTimestamp(value) {
  if (!Number.isFinite(value)) {
    return "—";
  }
  return new Intl.DateTimeFormat("zh-CN", {
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  }).format(new Date(value * 1000));
}

function joinPath(parent, name) {
  return parent === "/" ? `/${name}` : `${parent}/${name}`;
}

function showToast(message, error = false) {
  window.clearTimeout(toastTimer);
  toast.textContent = message;
  toast.classList.toggle("is-error", error);
  toast.hidden = false;
  toastTimer = window.setTimeout(() => {
    toast.hidden = true;
  }, 3200);
}

function setManagerBusy(busy, message = "") {
  managerBusy = busy;
  operationText.textContent = message;
  operationState.hidden = !busy;
  for (const button of filesView.querySelectorAll("button")) {
    button.disabled = busy;
  }
}

async function runManagerOperation(message, operation) {
  if (managerBusy) {
    return;
  }
  setManagerBusy(true, message);
  try {
    return await operation();
  } catch (error) {
    console.error(error);
    showToast(error.message || "操作失败", true);
    return null;
  } finally {
    setManagerBusy(false);
  }
}

function requestName(title, initialValue, submitText) {
  nameDialogTitle.textContent = title;
  nameInput.value = initialValue;
  nameSubmit.textContent = submitText;
  nameDialog.returnValue = "cancel";
  nameDialog.showModal();
  nameInput.focus();
  nameInput.select();
  return new Promise((resolve) => {
    nameDialog.addEventListener(
      "close",
      () => {
        resolve(
          nameDialog.returnValue === "default" ? nameInput.value.trim() : null,
        );
      },
      { once: true },
    );
  });
}

function requestConfirmation(title, message) {
  confirmTitle.textContent = title;
  confirmMessage.textContent = message;
  confirmDialog.returnValue = "cancel";
  confirmDialog.showModal();
  return new Promise((resolve) => {
    confirmDialog.addEventListener(
      "close",
      () => resolve(confirmDialog.returnValue === "confirm"),
      { once: true },
    );
  });
}

function makeIcon(name) {
  const icon = document.createElement("i");
  icon.dataset.lucide = name;
  return icon;
}

function makeRowAction(label, iconName, action, danger = false) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `row-action${danger ? " danger-action" : ""}`;
  button.title = label;
  button.setAttribute("aria-label", label);
  button.append(makeIcon(iconName));
  button.addEventListener("click", action);
  return button;
}

function renderBreadcrumbs(path) {
  breadcrumbs.replaceChildren();
  const rootButton = document.createElement("button");
  rootButton.type = "button";
  rootButton.textContent = "根目录";
  rootButton.addEventListener("click", () => openDirectory("/"));
  breadcrumbs.append(rootButton);

  let accumulated = "";
  for (const segment of path.split("/").filter(Boolean)) {
    accumulated += `/${segment}`;
    const separator = document.createElement("span");
    separator.className = "breadcrumb-separator";
    separator.append(makeIcon("chevron-right"));
    breadcrumbs.append(separator);

    const button = document.createElement("button");
    button.type = "button";
    button.textContent = segment;
    const target = accumulated;
    button.addEventListener("click", () => openDirectory(target));
    breadcrumbs.append(button);
  }
}

function renderFileList(payload) {
  currentPath = payload.path;
  currentEntries = payload.entries;
  fileList.replaceChildren();

  for (const entry of payload.entries) {
    const path = joinPath(payload.path, entry.name);
    const row = document.createElement("tr");

    const nameCell = document.createElement("td");
    const nameButton = document.createElement("button");
    nameButton.type = "button";
    nameButton.className = "file-name";
    nameButton.append(
      makeIcon(entry.directory ? "folder" : "file"),
      document.createTextNode(entry.name),
    );
    if (entry.directory) {
      nameButton.addEventListener("click", () => openDirectory(path));
    }
    nameCell.append(nameButton);

    const sizeCell = document.createElement("td");
    sizeCell.textContent = entry.directory ? "—" : formatBytes(entry.size);
    sizeCell.className = "file-size";

    const modifiedCell = document.createElement("td");
    modifiedCell.textContent = formatTimestamp(entry.modified);
    modifiedCell.className = "file-modified";

    const actionCell = document.createElement("td");
    actionCell.className = "row-actions";
    if (!entry.directory) {
      const download = document.createElement("a");
      download.className = "row-action";
      download.title = "下载";
      download.setAttribute("aria-label", `下载 ${entry.name}`);
      download.href = `/api/nand/download?path=${encodeURIComponent(path)}`;
      download.append(makeIcon("download"));
      actionCell.append(download);
    }
    actionCell.append(
      makeRowAction("重命名", "pencil", async () => {
        const name = await requestName("重命名", entry.name, "重命名");
        if (!name || name === entry.name) {
          return;
        }
        await runManagerOperation("正在重命名", async () => {
          await apiRequest("/api/nand/rename", {
            method: "POST",
            body: JSON.stringify({ path, name }),
          });
          await loadDirectory();
          showToast("重命名完成");
        });
      }),
      makeRowAction(
        "删除",
        "trash-2",
        async () => {
          const confirmed = await requestConfirmation(
            "删除文件",
            entry.directory
              ? `删除目录“${entry.name}”及其中全部内容？`
              : `删除文件“${entry.name}”？`,
          );
          if (!confirmed) {
            return;
          }
          await runManagerOperation("正在删除", async () => {
            await apiRequest("/api/nand/delete", {
              method: "POST",
              body: JSON.stringify({ path }),
            });
            await loadDirectory();
            showToast("删除完成");
          });
        },
        true,
      ),
    );

    row.append(nameCell, sizeCell, modifiedCell, actionCell);
    fileList.append(row);
  }

  const used = payload.capacityBytes - payload.freeBytes;
  const percentage =
    payload.capacityBytes > 0 ? (used / payload.capacityBytes) * 100 : 0;
  capacityUsed.textContent = `${formatBytes(used)} 已用`;
  capacityFree.textContent = `${formatBytes(payload.freeBytes)} 可用`;
  capacityProgress.value = percentage;
  capacityProgress.textContent = `${percentage.toFixed(0)}%`;
  changeState.textContent = payload.dirty ? "有未应用更改" : "未修改";
  changeState.classList.toggle("is-dirty", payload.dirty);
  emptyState.hidden = payload.entries.length !== 0;
  renderBreadcrumbs(payload.path);
  renderIcons();
}

async function loadDirectory(path = currentPath) {
  const payload = await apiRequest(
    `/api/nand/list?path=${encodeURIComponent(path)}`,
  );
  renderFileList(payload);
}

async function openDirectory(path) {
  await runManagerOperation("正在读取目录", () => loadDirectory(path));
}

function renderManagerStatus(status) {
  managerStatus = status;
  setPowerState(status);
  maintenanceGate.hidden = status.maintenance;
  fileBrowser.hidden = !status.maintenance;
  fileActions.hidden = !status.maintenance;
  gateTitle.textContent = status.emulatorRunning
    ? "NAND 正由模拟器使用"
    : "模拟器当前未运行";
  changeState.textContent = status.dirty ? "有未应用更改" : "未修改";
  changeState.classList.toggle("is-dirty", status.dirty);
}

async function refreshManagerStatus(loadFiles = false) {
  if (managerBusy) {
    return;
  }
  try {
    const previousMaintenance = managerStatus?.maintenance;
    const status = await apiRequest("/api/status");
    renderManagerStatus(status);
    if (
      status.maintenance &&
      (loadFiles || previousMaintenance !== status.maintenance)
    ) {
      await loadDirectory(currentPath);
    }
  } catch (error) {
    console.error(error);
    showToast("文件管理服务不可用", true);
  }
}

function disconnectForMaintenance() {
  window.clearTimeout(reconnectTimer);
  manualDisconnect = true;
  rfb?.disconnect();
  rfb = null;
  setConnectionState("offline", "NAND 维护");
}

async function switchView(view) {
  for (const tab of viewTabs) {
    const active = tab.dataset.view === view;
    tab.classList.toggle("is-active", active);
    tab.setAttribute("aria-selected", String(active));
  }
  for (const panel of viewPanels) {
    panel.hidden = panel.dataset.viewPanel !== view;
  }
  if (view === "files") {
    await refreshManagerStatus(true);
  }
}

for (const tab of viewTabs) {
  tab.addEventListener("click", () => switchView(tab.dataset.view));
}

document.querySelector("#open-nand").addEventListener("click", async () => {
  disconnectForMaintenance();
  const result = await runManagerOperation("正在停止 QEMU 并提取 NAND", () =>
    apiRequest("/api/nand/open", { method: "POST" }),
  );
  if (!result) {
    connect();
    return;
  }
  renderManagerStatus(result);
  await runManagerOperation("正在读取目录", () => loadDirectory("/"));
});

document.querySelector("#apply-nand").addEventListener("click", async () => {
  const confirmed = await requestConfirmation(
    "应用 NAND 更改",
    "将暂存文件回包为原始 NAND，并重新启动模拟器？",
  );
  if (!confirmed) {
    return;
  }
  const result = await runManagerOperation("正在回包并重启 QEMU", () =>
    apiRequest("/api/nand/apply", { method: "POST" }),
  );
  if (!result) {
    return;
  }
  renderManagerStatus(result);
  showToast("NAND 更改已应用");
  await switchView("emulator");
  connect();
});

document.querySelector("#discard-nand").addEventListener("click", async () => {
  const confirmed = await requestConfirmation(
    "放弃 NAND 更改",
    "放弃维护模式中的全部更改，并重新启动模拟器？",
  );
  if (!confirmed) {
    return;
  }
  const result = await runManagerOperation("正在放弃更改并重启 QEMU", () =>
    apiRequest("/api/nand/cancel", { method: "POST" }),
  );
  if (!result) {
    return;
  }
  renderManagerStatus(result);
  showToast("更改已放弃");
  await switchView("emulator");
  connect();
});

document
  .querySelector("#restart-emulator")
  .addEventListener("click", async () => {
    const result = await runManagerOperation("正在重启 QEMU", () =>
      apiRequest("/api/emulator/restart", { method: "POST" }),
    );
    if (result) {
      renderManagerStatus(result);
      showToast("模拟器已重启");
      await switchView("emulator");
      connect();
    }
  });

document.querySelector("#file-refresh").addEventListener("click", () => {
  runManagerOperation("正在刷新", () => loadDirectory());
});

document.querySelector("#new-folder").addEventListener("click", async () => {
  const name = await requestName("新建目录", "", "创建");
  if (!name) {
    return;
  }
  await runManagerOperation("正在创建目录", async () => {
    await apiRequest("/api/nand/mkdir", {
      method: "POST",
      body: JSON.stringify({ parent: currentPath, name }),
    });
    await loadDirectory();
    showToast("目录已创建");
  });
});

document.querySelector("#upload-file").addEventListener("click", () => {
  fileInput.click();
});

fileInput.addEventListener("change", async () => {
  const files = [...fileInput.files];
  fileInput.value = "";
  if (!files.length) {
    return;
  }

  await runManagerOperation(`正在上传 ${files.length} 个文件`, async () => {
    const existing = new Set(currentEntries.map((entry) => entry.name));
    for (const file of files) {
      if (existing.has(file.name)) {
        const overwrite = await requestConfirmation(
          "覆盖文件",
          `“${file.name}”已经存在，是否覆盖？`,
        );
        if (!overwrite) {
          continue;
        }
      }
      await apiRequest(
        `/api/nand/upload?path=${encodeURIComponent(currentPath)}&name=${encodeURIComponent(file.name)}`,
        { method: "PUT", body: file },
      );
      existing.add(file.name);
    }
    await loadDirectory();
    showToast("上传完成");
  });
});

window.setInterval(() => {
  if (document.querySelector('[data-view="files"]').classList.contains("is-active")) {
    refreshManagerStatus(false);
    return;
  }
  apiRequest("/api/status")
    .then((status) => {
      managerStatus = status;
      setPowerState(status);
      if (!status.emulatorRunning && !status.maintenance && !powerBusy) {
        setConnectionState("offline", "已关机");
      }
    })
    .catch((error) => console.error(error));
}, 3000);

apiRequest("/api/status")
  .then((status) => {
    managerStatus = status;
    setPowerState(status);
    if (!status.emulatorRunning && !status.maintenance) {
      setConnectionState("offline", "已关机");
    }
  })
  .catch((error) => console.error(error));
