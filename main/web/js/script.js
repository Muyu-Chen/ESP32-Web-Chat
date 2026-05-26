const loginContainer = document.getElementById('login-container');
const chatContainer = document.getElementById('chat-container');
const conversationListContainer = document.getElementById('conversation-list-container');
const createGroupModal = document.getElementById('create-group-modal');
const settingsModal = document.getElementById('settings-modal');

const joinBtn = document.getElementById('join-btn');
const nicknameInput = document.getElementById('nickname');
const chatNickname = document.getElementById('chat-nickname');
const loginDeviceId = document.getElementById('login-device-id');
const themeSwitch = document.getElementById('checkbox');
const connectionStatus = document.getElementById('connection-status');
const chatTitle = document.getElementById('chat-title');

const messages = document.getElementById('messages');
const form = document.getElementById('form');
const input = document.getElementById('message-input');

const menuBtn = document.getElementById('menu-btn');
const backBtn = document.getElementById('back-btn');
const createGroupBtn = document.getElementById('create-group-btn');
const createGroupConfirm = document.getElementById('create-group-confirm');
const createGroupCancel = document.getElementById('create-group-cancel');
const settingsBtn = document.getElementById('settings-btn');
const settingsAdminPassword = document.getElementById('settings-admin-password');
const settingsSsid = document.getElementById('settings-ssid');
const settingsWifiPassword = document.getElementById('settings-wifi-password');
const settingsOpenNetwork = document.getElementById('settings-open-network');
const settingsChannel = document.getElementById('settings-channel');
const settingsNewAdminPassword = document.getElementById('settings-new-admin-password');
const settingsStatus = document.getElementById('settings-status');
const settingsSave = document.getElementById('settings-save');
const settingsSaveReboot = document.getElementById('settings-save-reboot');
const settingsCancel = document.getElementById('settings-cancel');
const conversationList = document.getElementById('conversation-list');
const userSelectList = document.getElementById('user-select-list');
const recoverHistoryBtn = document.getElementById('recover-history-btn');
const historyRecoveryStatus = document.getElementById('history-recovery-status');

const STORAGE = {
    userId: 'esp-chat-uuid',
    nickname: 'esp-chat-nickname',
    messages: 'esp-chat-messages',
    conversations: 'esp-chat-conversations',
    lastSeenId: 'esp-chat-last-seen-id',
    outbox: 'esp-chat-outbox',
    theme: 'theme'
};

const MAX_LOCAL_MESSAGES = 2000;
const MAX_OUTBOX_MESSAGES = 30;
const MAX_SAFE_MESSAGE_ID = Number.MAX_SAFE_INTEGER;
const HISTORY_RECOVERY_WINDOW_MS = 4000;
const DEFAULT_AP_HOST = '192.168.4.1';
const WS_FALLBACK_DELAY_MS = 250;

let ws = null;
let hasJoined = false;
let reconnectTimer = null;
let reconnectDelayMs = 1000;
let wsUrlAttempt = 0;
let allMessages = [];
let conversations = {};
let onlineUsers = new Map();
let outbox = [];
let lastSeenId = 0;
let historyInfo = null;
let activeRecovery = null;

function generateUUID() {
    let d = new Date().getTime();
    let d2 = ((typeof performance !== 'undefined') && performance.now && (performance.now() * 1000)) || 0;
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
        let r = Math.random() * 16;
        if (d > 0) {
            r = (d + r) % 16 | 0;
            d = Math.floor(d / 16);
        } else {
            r = (d2 + r) % 16 | 0;
            d2 = Math.floor(d2 / 16);
        }
        return (c === 'x' ? r : (r & 0x3 | 0x8)).toString(16);
    });
}

function readJSON(key, fallback) {
    try {
        const raw = localStorage.getItem(key);
        return raw ? JSON.parse(raw) : fallback;
    } catch (error) {
        console.warn(`Failed to parse ${key}:`, error);
        return fallback;
    }
}

function writeJSON(key, value) {
    try {
        localStorage.setItem(key, JSON.stringify(value));
    } catch (error) {
        console.warn(`Failed to store ${key}:`, error);
    }
}

let userId = localStorage.getItem(STORAGE.userId);
if (!userId) {
    userId = generateUUID();
    localStorage.setItem(STORAGE.userId, userId);
}

nicknameInput.value = localStorage.getItem(STORAGE.nickname) || `User${Math.floor(Math.random() * 1000)}`;
loginDeviceId.textContent = `Device ID: ${userId.slice(0, 8)}`;

function defaultConversation() {
    return {
        id: 'global',
        name: 'Group Chat',
        type: 'global',
        members: [],
        lastPreview: 'Everyone connected to this ESP32',
        updatedAt: 0
    };
}

function loadState() {
    allMessages = readJSON(STORAGE.messages, []).filter((msg) => msg && isSafeMessageId(msg.id));
    conversations = readJSON(STORAGE.conversations, {});
    conversations.global = { ...defaultConversation(), ...(conversations.global || {}) };
    outbox = readJSON(STORAGE.outbox, []);
    lastSeenId = Number(localStorage.getItem(STORAGE.lastSeenId) || 0);

    const localMaxId = allMessages
        .filter((msg) => !msg.recovered)
        .reduce((maxId, msg) => Math.max(maxId, Number(msg.id) || 0), 0);
    lastSeenId = Math.max(lastSeenId, localMaxId);
    localStorage.setItem(STORAGE.lastSeenId, String(lastSeenId));
}

let currentConversation = defaultConversation();
loadState();
currentConversation = conversations.global;

function saveMessages() {
    const sorted = allMessages.sort((a, b) => Number(a.id) - Number(b.id));
    if (sorted.length > MAX_LOCAL_MESSAGES) {
        const recovered = sorted.filter((msg) => msg.recovered).slice(-MAX_LOCAL_MESSAGES);
        const regular = sorted.filter((msg) => !msg.recovered);
        const regularSlots = MAX_LOCAL_MESSAGES - recovered.length;
        allMessages = [
            ...(regularSlots > 0 ? regular.slice(-regularSlots) : []),
            ...recovered
        ].sort((a, b) => Number(a.id) - Number(b.id));
    } else {
        allMessages = sorted;
    }
    writeJSON(STORAGE.messages, allMessages);
}

function saveConversations() {
    writeJSON(STORAGE.conversations, conversations);
}

function saveOutbox() {
    outbox = outbox.slice(-MAX_OUTBOX_MESSAGES);
    writeJSON(STORAGE.outbox, outbox);
}

function rememberSeenId(id) {
    const numericId = Number(id);
    if (Number.isSafeInteger(numericId) && numericId > lastSeenId) {
        lastSeenId = numericId;
        localStorage.setItem(STORAGE.lastSeenId, String(lastSeenId));
    }
}

function setStatus(state, label) {
    connectionStatus.dataset.state = state;
    connectionStatus.textContent = label;
}

function getNickname() {
    return (localStorage.getItem(STORAGE.nickname) || nicknameInput.value || 'Guest').trim().slice(0, 31);
}

function shortUserId(id) {
    return String(id || '').slice(0, 8);
}

function targetUsers(msg) {
    return msg && msg.to && Array.isArray(msg.to.users) ? msg.to.users : [];
}

function isMessageForMe(msg) {
    if (!msg || msg.from === 'server') {
        return true;
    }
    if (msg.from === userId || (msg.to && msg.to.all)) {
        return true;
    }
    return targetUsers(msg).includes(userId);
}

function isMessageVisibleToUser(msg, targetUserId) {
    if (!msg || !targetUserId) {
        return false;
    }
    if (msg.from === targetUserId || (msg.to && msg.to.all)) {
        return true;
    }
    return targetUsers(msg).includes(targetUserId);
}

function isSafeMessageId(id) {
    const numericId = Number(id);
    return Number.isSafeInteger(numericId) && numericId > 0 && numericId <= MAX_SAFE_MESSAGE_ID;
}

function validUserId(value) {
    return typeof value === 'string' && value.length > 0 && value.length <= 63;
}

function validMessageTarget(to) {
    return to &&
        typeof to === 'object' &&
        typeof to.all === 'boolean' &&
        Array.isArray(to.users) &&
        to.users.length <= 17 &&
        to.users.every(validUserId) &&
        (to.all || to.users.length > 0);
}

function validHistoryMessage(msg) {
    if (!msg || typeof msg !== 'object' || !isSafeMessageId(msg.id)) {
        return false;
    }
    if (msg.type !== 'text' && msg.type !== 'newGroup') {
        return false;
    }
    if (!validUserId(msg.from) || typeof msg.name !== 'string' || msg.name.length === 0 || msg.name.length > 31) {
        return false;
    }
    if (!validMessageTarget(msg.to) || !Number.isFinite(Number(msg.timestamp))) {
        return false;
    }
    if (typeof msg.data !== 'string' || msg.data.length > 256) {
        return false;
    }
    if (msg.type === 'text' && msg.data.length === 0) {
        return false;
    }
    if (msg.type === 'newGroup') {
        return typeof msg.groupId === 'string' && msg.groupId.length > 0 && msg.groupId.length <= 63 &&
            typeof msg.groupName === 'string' && msg.groupName.length > 0 && msg.groupName.length <= 63;
    }
    return true;
}

function comparableMessage(msg) {
    return JSON.stringify({
        id: Number(msg.id),
        type: msg.type,
        from: msg.from,
        name: msg.name,
        to: {
            all: Boolean(msg.to && msg.to.all),
            users: targetUsers(msg).slice().sort()
        },
        data: msg.data || '',
        timestamp: Number(msg.timestamp) || 0,
        groupId: msg.groupId || '',
        groupName: msg.groupName || ''
    });
}

function conversationForMessage(msg) {
    if (msg.type === 'newGroup' || msg.groupId) {
        return msg.groupId;
    }
    if (msg.to && msg.to.all) {
        return 'global';
    }
    if (msg.from === userId) {
        return targetUsers(msg).find((id) => id !== userId) || 'global';
    }
    return msg.from || 'global';
}

function updateConversationFromMessage(msg) {
    if (!msg || msg.type === 'error' || msg.type === 'onlineUsers') {
        return;
    }

    const conversationId = conversationForMessage(msg);
    const preview = msg.type === 'newGroup' ? msg.data : msg.data;
    const updatedAt = Number(msg.timestamp) || Date.now() / 1000;
    const existingConversation = conversations[conversationId];
    const keepExistingPreview = existingConversation && (existingConversation.updatedAt || 0) > updatedAt;

    if (conversationId === 'global') {
        conversations.global = {
            ...defaultConversation(),
            ...(conversations.global || {}),
            lastPreview: keepExistingPreview ? conversations.global.lastPreview : (preview || 'Group Chat'),
            updatedAt: keepExistingPreview ? conversations.global.updatedAt : updatedAt
        };
        return;
    }

    if (msg.type === 'newGroup' || msg.groupId) {
        const members = Array.from(new Set([msg.from, ...targetUsers(msg)].filter(Boolean)));
        conversations[conversationId] = {
            id: conversationId,
            name: msg.groupName || conversations[conversationId]?.name || 'Group Chat',
            type: 'group',
            members,
            lastPreview: keepExistingPreview ? existingConversation.lastPreview : (preview || 'New group'),
            updatedAt: keepExistingPreview ? existingConversation.updatedAt : updatedAt
        };
        return;
    }

    const otherId = conversationId;
    const otherName = msg.from === userId
        ? onlineUsers.get(otherId)?.name || conversations[otherId]?.name || otherId.slice(0, 8)
        : msg.name || conversations[otherId]?.name || otherId.slice(0, 8);

    conversations[otherId] = {
        id: otherId,
        name: otherName,
        type: 'private',
        members: [userId, otherId],
        lastPreview: keepExistingPreview ? existingConversation.lastPreview : (preview || 'Private chat'),
        updatedAt: keepExistingPreview ? existingConversation.updatedAt : updatedAt
    };
}

function saveIncomingMessage(msg) {
    rememberSeenId(msg.id);
    if (!isMessageForMe(msg)) {
        return false;
    }

    const id = Number(msg.id);
    if (!isSafeMessageId(id) || allMessages.some((stored) => Number(stored.id) === id)) {
        return false;
    }

    allMessages.push(msg);
    updateConversationFromMessage(msg);
    saveMessages();
    saveConversations();
    return true;
}

function belongsToCurrentConversation(msg) {
    if (currentConversation.type === 'global') {
        return msg.type === 'text' && msg.to && msg.to.all;
    }
    if (currentConversation.type === 'group') {
        return msg.groupId === currentConversation.id || (msg.type === 'newGroup' && msg.groupId === currentConversation.id);
    }
    if (currentConversation.type === 'private') {
        return !msg.groupId &&
            !(msg.to && msg.to.all) &&
            ((msg.from === userId && targetUsers(msg).includes(currentConversation.id)) ||
             (msg.from === currentConversation.id && targetUsers(msg).includes(userId)));
    }
    return false;
}

function formatTime(timestamp) {
    const ts = Number(timestamp);
    if (!Number.isFinite(ts) || ts <= 0) {
        return '';
    }
    return new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function createSystemElement(text) {
    const item = document.createElement('div');
    item.className = 'system';
    item.textContent = text;
    return item;
}

function createMessageElement(msg) {
    if (msg.type === 'newGroup') {
        const text = msg.data || `${msg.name || 'Someone'} created ${msg.groupName || 'a group'}`;
        const system = createSystemElement(msg.recovered ? `${text} · Recovered from ${msg.recoveredFromName || 'another device'}` : text);
        if (msg.recovered) {
            system.classList.add('system-recovered');
        }
        return system;
    }

    const isMine = msg.from === userId;
    const item = document.createElement('div');
    item.className = `message-row${isMine ? ' message-row-mine' : ''}`;

    const bubble = document.createElement('div');
    bubble.className = `message-bubble${isMine ? ' message-bubble-mine' : ''}`;
    if (msg.recovered) {
        bubble.classList.add('message-bubble-recovered');
    }

    const from = document.createElement('div');
    from.className = 'from';
    from.textContent = isMine ? 'Me' : (msg.name || 'Guest');

    const data = document.createElement('div');
    data.className = 'message-content';
    data.textContent = msg.data || '';

    const timestamp = document.createElement('span');
    timestamp.className = 'timestamp';
    timestamp.textContent = formatTime(msg.timestamp);

    bubble.appendChild(from);
    bubble.appendChild(data);
    if (msg.recovered) {
        const recovered = document.createElement('span');
        recovered.className = 'recovered-label';
        recovered.textContent = `Recovered from ${msg.recoveredFromName || 'another device'}`;
        bubble.appendChild(recovered);
    }
    bubble.appendChild(timestamp);
    item.appendChild(bubble);
    return item;
}

function renderMessages() {
    messages.innerHTML = '';
    chatTitle.textContent = currentConversation.name;

    const visibleMessages = allMessages
        .filter(belongsToCurrentConversation)
        .sort((a, b) => Number(a.id) - Number(b.id));

    if (visibleMessages.length === 0) {
        messages.appendChild(createSystemElement('No messages yet.'));
        return;
    }

    visibleMessages.forEach((msg) => messages.appendChild(createMessageElement(msg)));
    messages.scrollTop = messages.scrollHeight;
}

function showSystemMessage(text) {
    messages.appendChild(createSystemElement(text));
    messages.scrollTop = messages.scrollHeight;
}

function conversationSubtitle(conversation) {
    if (conversation.type === 'global') {
        const count = otherOnlineUserCount();
        return `${count} friend${count === 1 ? '' : 's'} online`;
    }
    if (conversation.type === 'private') {
        return `${onlineUsers.has(conversation.id) ? 'Online' : 'Offline'} • ${shortUserId(conversation.id)}`;
    }
    return `${conversation.members?.length || 0} members`;
}

function addConversationSection(title) {
    const section = document.createElement('div');
    section.className = 'conversation-section';
    section.textContent = title;
    conversationList.appendChild(section);
}

function addConversationItem(conversation) {
    const item = document.createElement('button');
    item.type = 'button';
    item.className = 'conversation-item';
    if (conversation.id === currentConversation.id) {
        item.classList.add('conversation-item-active');
    }
    if (conversation.type === 'private' && onlineUsers.has(conversation.id)) {
        item.classList.add('conversation-item-online');
    }

    const title = document.createElement('strong');
    title.textContent = conversation.name;

    const meta = document.createElement('span');
    meta.textContent = conversation.lastPreview || conversationSubtitle(conversation);

    const subtitle = document.createElement('small');
    subtitle.textContent = conversationSubtitle(conversation);

    item.appendChild(title);
    item.appendChild(meta);
    item.appendChild(subtitle);
    item.addEventListener('click', () => {
        currentConversation = conversation;
        chatContainer.classList.add('visible');
        conversationListContainer.classList.remove('visible');
        renderMessages();
        renderConversationList();
    });

    conversationList.appendChild(item);
}

function renderConversationList() {
    conversationList.innerHTML = '';

    addConversationSection(`Me: ${getNickname()} (${userId.slice(0, 8)})`);
    addConversationItem(conversations.global);

    const otherUsers = Array.from(onlineUsers.values()).filter((user) => user.id !== userId);
    addConversationSection(`Online friends (${otherUsers.length})`);
    if (otherUsers.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'conversation-empty';
        empty.textContent = 'No other users online yet.';
        conversationList.appendChild(empty);
    } else {
        otherUsers
            .sort((a, b) => a.name.localeCompare(b.name))
            .forEach((user) => {
                conversations[user.id] = {
                    id: user.id,
                    name: user.name || user.id.slice(0, 8),
                    type: 'private',
                    members: [userId, user.id],
                    lastPreview: conversations[user.id]?.lastPreview || 'Tap to start a private chat',
                    updatedAt: conversations[user.id]?.updatedAt || 0
                };
                addConversationItem(conversations[user.id]);
            });
    }

    const history = Object.values(conversations)
        .filter((conversation) => conversation.id !== 'global')
        .filter((conversation) => conversation.type === 'group' || !onlineUsers.has(conversation.id))
        .sort((a, b) => (b.updatedAt || 0) - (a.updatedAt || 0));

    if (history.length > 0) {
        addConversationSection('History');
        history.forEach(addConversationItem);
    }

    saveConversations();
}

function updateOnlineUsers(data) {
    onlineUsers = new Map();
    if (Array.isArray(data)) {
        data.forEach((user) => {
            if (user && user.id) {
                onlineUsers.set(user.id, {
                    id: user.id,
                    name: user.name || user.id.slice(0, 8)
                });
            }
        });
    }

    if (!onlineUsers.has(userId)) {
        onlineUsers.set(userId, { id: userId, name: getNickname() });
    }

    renderConversationList();
    updateRecoveryControls();
}

function sendRaw(payload) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(payload));
        return true;
    }
    return false;
}

function sendControl(type, extra = {}) {
    return sendRaw({
        type,
        from: userId,
        name: getNickname(),
        timestamp: Math.floor(Date.now() / 1000),
        ...extra
    });
}

function queueMessage(message) {
    outbox.push(message);
    saveOutbox();
    showSystemMessage('Message queued until the ESP32 connection returns.');
}

function flushOutbox() {
    if (!ws || ws.readyState !== WebSocket.OPEN || outbox.length === 0) {
        return;
    }

    const pending = [...outbox];
    outbox = [];
    saveOutbox();
    pending.forEach((message) => {
        message.timestamp = Math.floor(Date.now() / 1000);
        sendRaw(message);
    });
    showSystemMessage(`Sent ${pending.length} queued message${pending.length > 1 ? 's' : ''}.`);
}

function recoverySourceName(source) {
    return source?.name || onlineUsers.get(source?.from)?.name || (source?.from ? source.from.slice(0, 8) : 'another device');
}

function setRecoveryStatus(text) {
    if (historyRecoveryStatus) {
        historyRecoveryStatus.textContent = text || '';
    }
}

function otherOnlineUserCount() {
    return Array.from(onlineUsers.keys()).filter((id) => id !== userId).length;
}

function updateRecoveryControls() {
    if (!recoverHistoryBtn) {
        return;
    }

    const restoreBeforeId = Number(historyInfo?.restore_before_id || 0);
    const canRecover = ws &&
        ws.readyState === WebSocket.OPEN &&
        Number.isSafeInteger(restoreBeforeId) &&
        restoreBeforeId > 1 &&
        otherOnlineUserCount() > 0 &&
        !activeRecovery;

    recoverHistoryBtn.disabled = !canRecover;

    if (activeRecovery) {
        setRecoveryStatus(`Recovering before #${activeRecovery.restoreBeforeId}...`);
    } else if (!ws || ws.readyState !== WebSocket.OPEN) {
        setRecoveryStatus('Connect to recover old messages.');
    } else if (!historyInfo) {
        setRecoveryStatus('Waiting for history boundary.');
    } else if (restoreBeforeId <= 1) {
        setRecoveryStatus('No older server boundary.');
    } else if (otherOnlineUserCount() === 0) {
        setRecoveryStatus('No other online devices.');
    } else {
        setRecoveryStatus(`Can recover before #${restoreBeforeId}.`);
    }
}

function historyMessagePayload(msg) {
    const payload = {
        type: msg.type,
        from: msg.from,
        to: {
            all: Boolean(msg.to && msg.to.all),
            users: targetUsers(msg).filter(validUserId)
        },
        name: msg.name,
        data: msg.data || '',
        id: Number(msg.id),
        timestamp: Number(msg.timestamp) || 0
    };

    if (msg.type === 'newGroup') {
        payload.groupId = msg.groupId;
        payload.groupName = msg.groupName;
    }

    return payload;
}

function recoveredMessagePayload(msg, source) {
    const payload = historyMessagePayload(msg);
    payload.recovered = true;
    payload.recoveredFrom = source.from;
    payload.recoveredFromName = recoverySourceName(source);
    payload.recoveredAt = Math.floor(Date.now() / 1000);
    return payload;
}

function importRecoveredMessage(candidate, source) {
    if (!validHistoryMessage(candidate) || !isMessageForMe(candidate)) {
        return 'ignored';
    }

    const id = Number(candidate.id);
    const restoreBeforeId = Number(activeRecovery?.restoreBeforeId || historyInfo?.restore_before_id || 0);
    if (!Number.isSafeInteger(restoreBeforeId) || restoreBeforeId <= 1 || id >= restoreBeforeId) {
        return 'ignored';
    }

    const existing = allMessages.find((stored) => Number(stored.id) === id);
    if (existing) {
        return comparableMessage(existing) === comparableMessage(candidate) ? 'duplicate' : 'conflict';
    }

    const recovered = recoveredMessagePayload(candidate, source);
    allMessages.push(recovered);
    updateConversationFromMessage(recovered);
    saveMessages();
    saveConversations();
    return 'imported';
}

function finishHistoryRecovery() {
    if (!activeRecovery) {
        return;
    }

    const recovery = activeRecovery;
    activeRecovery = null;
    if (recovery.changed) {
        renderMessages();
        renderConversationList();
    }

    const parts = [`Recovered ${recovery.imported} old message${recovery.imported === 1 ? '' : 's'}`];
    if (recovery.conflicts > 0) {
        parts.push(`${recovery.conflicts} conflict${recovery.conflicts === 1 ? '' : 's'} skipped`);
    }
    if (recovery.duplicates > 0) {
        parts.push(`${recovery.duplicates} duplicate${recovery.duplicates === 1 ? '' : 's'} ignored`);
    }
    showSystemMessage(`${parts.join(', ')}.`);
    updateRecoveryControls();
    setRecoveryStatus(`${parts.join(', ')}.`);
}

function startHistoryRecovery() {
    if (activeRecovery) {
        return;
    }

    const restoreBeforeId = Number(historyInfo?.restore_before_id || 0);
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        setRecoveryStatus('Connect to the ESP32 first.');
        return;
    }
    if (!Number.isSafeInteger(restoreBeforeId) || restoreBeforeId <= 1) {
        setRecoveryStatus('No older server boundary to recover.');
        return;
    }
    if (otherOnlineUserCount() === 0) {
        setRecoveryStatus('No other online devices to ask.');
        return;
    }

    const requestId = `hist-${generateUUID()}`;
    const recovery = {
        requestId,
        restoreBeforeId,
        imported: 0,
        duplicates: 0,
        conflicts: 0,
        ignored: 0,
        changed: false,
        timer: null
    };

    if (!sendControl('historyRequest', { requestId, restore_before_id: restoreBeforeId })) {
        setRecoveryStatus('Unable to send recovery request.');
        return;
    }

    activeRecovery = recovery;
    recovery.timer = setTimeout(finishHistoryRecovery, HISTORY_RECOVERY_WINDOW_MS);
    updateRecoveryControls();
}

function handleHistoryInfo(msg) {
    const restoreBeforeId = Number(msg.restore_before_id);
    if (!Number.isSafeInteger(restoreBeforeId) || restoreBeforeId < 0) {
        return;
    }

    historyInfo = {
        boot_start_id: Number(msg.boot_start_id) || 0,
        current_id: Number(msg.current_id) || 0,
        earliest_id: Number(msg.earliest_id) || 0,
        latest_id: Number(msg.latest_id) || 0,
        restore_before_id: restoreBeforeId,
        count: Number(msg.count) || 0,
        capacity: Number(msg.capacity) || 0,
        has_more_before: Boolean(msg.has_more_before)
    };
    updateRecoveryControls();
}

function handleHistoryRequest(msg) {
    if (msg.from === userId ||
        !validUserId(msg.from) ||
        typeof msg.requestId !== 'string' ||
        msg.requestId.length === 0 ||
        msg.requestId.length > 63) {
        return;
    }

    const restoreBeforeId = Number(msg.restore_before_id);
    if (!Number.isSafeInteger(restoreBeforeId) || restoreBeforeId <= 1) {
        return;
    }

    allMessages
        .filter((stored) => !stored.recovered)
        .filter((stored) => validHistoryMessage(stored))
        .filter((stored) => Number(stored.id) < restoreBeforeId)
        .filter((stored) => isMessageVisibleToUser(stored, msg.from))
        .sort((a, b) => Number(a.id) - Number(b.id))
        .forEach((stored) => {
            sendControl('historyResponse', {
                requestId: msg.requestId.slice(0, 63),
                to: { all: false, users: [msg.from] },
                message: historyMessagePayload(stored)
            });
        });
}

function handleHistoryResponse(msg) {
    if (!activeRecovery ||
        msg.requestId !== activeRecovery.requestId ||
        msg.from === userId ||
        !validUserId(msg.from) ||
        typeof msg.name !== 'string') {
        return;
    }
    if (!targetUsers(msg).includes(userId)) {
        return;
    }

    const result = importRecoveredMessage(msg.message, msg);
    if (result === 'imported') {
        activeRecovery.imported++;
        activeRecovery.changed = true;
    } else if (result === 'conflict') {
        activeRecovery.conflicts++;
    } else if (result === 'duplicate') {
        activeRecovery.duplicates++;
    } else {
        activeRecovery.ignored++;
    }
}

function handleIncoming(event) {
    try {
        const msg = JSON.parse(event.data);

        if (msg.type === 'ping') {
            sendRaw({ type: 'pong', from: userId, timestamp: Math.floor(Date.now() / 1000) });
            return;
        }

        if (Number.isSafeInteger(Number(msg.id))) {
            rememberSeenId(msg.id);
        }

        if (msg.type === 'historyInfo') {
            handleHistoryInfo(msg);
            return;
        }

        if (msg.type === 'historyRequest') {
            handleHistoryRequest(msg);
            return;
        }

        if (msg.type === 'historyResponse') {
            handleHistoryResponse(msg);
            return;
        }

        if (msg.type === 'onlineUsers') {
            updateOnlineUsers(msg.data);
            return;
        }

        if (msg.type === 'error') {
            showSystemMessage(`Server error: ${msg.data || msg.code || 'unknown error'}`);
            return;
        }

        if (msg.type !== 'text' && msg.type !== 'newGroup') {
            return;
        }

        const saved = saveIncomingMessage(msg);
        if (saved) {
            if (msg.type === 'newGroup' && msg.groupId && (msg.from === userId || targetUsers(msg).includes(userId))) {
                currentConversation = conversations[msg.groupId] || currentConversation;
            }
            renderMessages();
            renderConversationList();
        }
    } catch (error) {
        console.error('Error parsing WebSocket message:', error);
    }
}

function scheduleReconnect() {
    if (reconnectTimer) {
        return;
    }

    setStatus('offline', `Reconnecting in ${Math.round(reconnectDelayMs / 1000)}s`);
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        reconnectDelayMs = Math.min(reconnectDelayMs * 2, 15000);
        connect();
    }, reconnectDelayMs);
}

function websocketUrls() {
    const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const hosts = [];
    const currentHost = window.location.host || DEFAULT_AP_HOST;

    if (currentHost) {
        hosts.push(currentHost);
    }
    if (!hosts.some((host) => host.split(':')[0] === DEFAULT_AP_HOST)) {
        hosts.push(DEFAULT_AP_HOST);
    }

    return hosts.map((host) => `${protocol}://${host}/ws`);
}

function connect() {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        return;
    }

    setStatus('connecting', 'Connecting...');
    const urls = websocketUrls();
    const url = urls[Math.min(wsUrlAttempt, urls.length - 1)];
    let opened = false;

    try {
        ws = new WebSocket(url);
    } catch (error) {
        ws = null;
        if (wsUrlAttempt < urls.length - 1) {
            wsUrlAttempt += 1;
            setStatus('connecting', 'Trying ESP32 address...');
            setTimeout(connect, WS_FALLBACK_DELAY_MS);
        } else {
            wsUrlAttempt = 0;
            setStatus('offline', 'Connection error');
            scheduleReconnect();
        }
        return;
    }

    ws.onopen = () => {
        opened = true;
        wsUrlAttempt = 0;
        hasJoined = true;
        reconnectDelayMs = 1000;
        setStatus('online', 'Connected');
        updateRecoveryControls();
        sendControl('join', { since_id: lastSeenId });
        sendControl('getOnlineUser');
        flushOutbox();
    };

    ws.onmessage = handleIncoming;

    ws.onclose = () => {
        ws = null;
        if (activeRecovery) {
            clearTimeout(activeRecovery.timer);
            activeRecovery = null;
            setRecoveryStatus('Recovery interrupted.');
        }
        updateRecoveryControls();
        if (!opened && wsUrlAttempt < urls.length - 1) {
            wsUrlAttempt += 1;
            setStatus('connecting', 'Trying ESP32 address...');
            setTimeout(connect, WS_FALLBACK_DELAY_MS);
            return;
        }
        wsUrlAttempt = 0;
        setStatus('offline', opened || hasJoined ? 'Disconnected' : 'Connection error');
        scheduleReconnect();
    };

    ws.onerror = () => {
        setStatus('offline', 'Connection error');
    };
}

function messageTargetForCurrentConversation() {
    if (currentConversation.type === 'global') {
        return { all: true, users: [] };
    }
    if (currentConversation.type === 'group') {
        const members = Array.from(new Set([userId, ...(currentConversation.members || [])]));
        return { all: false, users: members };
    }
    return { all: false, users: [currentConversation.id] };
}

function buildOutgoingMessage(text) {
    const message = {
        type: 'text',
        from: userId,
        to: messageTargetForCurrentConversation(),
        name: getNickname(),
        data: text,
        timestamp: Math.floor(Date.now() / 1000)
    };

    if (currentConversation.type === 'group') {
        message.groupId = currentConversation.id;
        message.groupName = currentConversation.name;
    }

    return message;
}

function sendMessage(event) {
    event.preventDefault();
    const text = input.value.trim();
    if (!text) {
        return;
    }

    const message = buildOutgoingMessage(text.slice(0, 256));
    input.value = '';

    if (!sendRaw(message)) {
        queueMessage(message);
    }
}

function openGroupModal() {
    userSelectList.innerHTML = '';
    const candidates = Array.from(onlineUsers.values()).filter((user) => user.id !== userId);

    if (candidates.length === 0) {
        const empty = document.createElement('div');
        empty.className = 'conversation-empty';
        empty.textContent = 'No online users available.';
        userSelectList.appendChild(empty);
    } else {
        candidates.forEach((user) => {
            const label = document.createElement('label');
            label.className = 'user-select-item';

            const checkbox = document.createElement('input');
            checkbox.type = 'checkbox';
            checkbox.value = user.id;

            const name = document.createElement('span');
            name.textContent = user.name;

            label.appendChild(checkbox);
            label.appendChild(name);
            userSelectList.appendChild(label);
        });
    }

    createGroupModal.classList.add('visible');
}

function closeGroupModal() {
    createGroupModal.classList.remove('visible');
}

function createGroup() {
    const selectedIds = Array.from(userSelectList.querySelectorAll('input[type="checkbox"]:checked')).map((input) => input.value);
    if (selectedIds.length === 0) {
        alert('Select at least one online user.');
        return;
    }

    const members = Array.from(new Set([userId, ...selectedIds]));
    const memberNames = members.map((id) => onlineUsers.get(id)?.name || (id === userId ? getNickname() : id.slice(0, 8)));
    const groupId = `group-${generateUUID()}`;
    const groupName = memberNames.join(', ');

    const message = {
        type: 'newGroup',
        from: userId,
        name: getNickname(),
        groupId,
        groupName,
        to: { all: false, users: members },
        data: `${getNickname()} created ${groupName}`,
        timestamp: Math.floor(Date.now() / 1000)
    };

    conversations[groupId] = {
        id: groupId,
        name: groupName,
        type: 'group',
        members,
        lastPreview: message.data,
        updatedAt: message.timestamp
    };
    currentConversation = conversations[groupId];
    saveConversations();
    closeGroupModal();
    renderMessages();
    renderConversationList();

    if (!sendRaw(message)) {
        queueMessage(message);
    }
}

function setSettingsStatus(text, isError = false) {
    settingsStatus.textContent = text;
    settingsStatus.dataset.state = isError ? 'error' : 'ok';
}

async function openSettings() {
    settingsStatus.textContent = '';
    settingsAdminPassword.value = '';
    settingsWifiPassword.value = '';
    settingsNewAdminPassword.value = '';
    settingsOpenNetwork.checked = false;
    settingsWifiPassword.disabled = false;
    settingsModal.classList.add('visible');
    setSettingsStatus('Loading current settings...');

    try {
        const response = await fetch('/api/settings', { cache: 'no-store' });
        const data = await response.json();
        if (!data.ok) {
            throw new Error(data.message || 'Unable to load settings');
        }
        settingsSsid.value = data.ssid || '';
        settingsChannel.value = data.channel || 1;
        settingsWifiPassword.placeholder = data.passwordSet
            ? 'Leave blank to keep current password'
            : 'No password is currently set';
        setSettingsStatus('Enter the admin password to save changes.');
    } catch (error) {
        setSettingsStatus(`Failed to load settings: ${error.message}`, true);
    }
}

function closeSettings() {
    settingsModal.classList.remove('visible');
}

async function saveSettings(reboot) {
    const adminPassword = settingsAdminPassword.value.trim();
    const ssid = settingsSsid.value.trim();
    const channel = Number(settingsChannel.value);
    const password = settingsWifiPassword.value;
    const newAdminPassword = settingsNewAdminPassword.value;

    if (!adminPassword) {
        setSettingsStatus('Admin password is required.', true);
        return;
    }
    if (!ssid) {
        setSettingsStatus('SSID cannot be empty.', true);
        return;
    }
    if (!Number.isInteger(channel) || channel < 1 || channel > 13) {
        setSettingsStatus('Channel must be between 1 and 13.', true);
        return;
    }
    if (!settingsOpenNetwork.checked && password && (password.length < 8 || password.length > 63)) {
        setSettingsStatus('Wi-Fi password must be 8 to 63 characters, or left blank to keep the current password.', true);
        return;
    }
    if (newAdminPassword && (newAdminPassword.length < 4 || newAdminPassword.length > 32)) {
        setSettingsStatus('New admin password must be 4 to 32 characters.', true);
        return;
    }

    setSettingsStatus(reboot ? 'Saving settings and restarting...' : 'Saving settings...');

    try {
        const response = await fetch('/api/settings', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                adminPassword,
                ssid,
                password,
                openNetwork: settingsOpenNetwork.checked,
                channel,
                newAdminPassword,
                reboot
            })
        });
        const data = await response.json();
        if (!data.ok) {
            throw new Error(data.message || 'Save failed');
        }
        setSettingsStatus(data.message || 'Settings saved.');
        if (data.restarting) {
            hasJoined = false;
            if (ws) {
                ws.close();
            }
            setStatus('offline', 'ESP32 restarting');
        }
    } catch (error) {
        setSettingsStatus(`Save failed: ${error.message}`, true);
    }
}

function enterChat() {
    const nickname = nicknameInput.value.trim().slice(0, 31);
    if (!nickname) {
        alert('Please enter a nickname.');
        return;
    }

    hasJoined = true;
    localStorage.setItem(STORAGE.nickname, nickname);
    chatNickname.textContent = nickname;
    onlineUsers.set(userId, { id: userId, name: nickname });

    loginContainer.style.opacity = '0';
    setTimeout(() => {
        loginContainer.style.display = 'none';
        chatContainer.classList.add('visible');
        renderMessages();
        renderConversationList();
        connect();
    }, 250);
}

function setTheme(isDark) {
    document.body.classList.toggle('dark-mode', isDark);
    themeSwitch.checked = isDark;
    localStorage.setItem(STORAGE.theme, isDark ? 'dark' : 'light');
}

joinBtn.addEventListener('click', enterChat);
nicknameInput.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
        enterChat();
    }
});

menuBtn.addEventListener('click', () => {
    renderConversationList();
    chatContainer.classList.remove('visible');
    conversationListContainer.classList.add('visible');
});

backBtn.addEventListener('click', () => {
    conversationListContainer.classList.remove('visible');
    chatContainer.classList.add('visible');
    renderMessages();
});

createGroupBtn.addEventListener('click', openGroupModal);
createGroupCancel.addEventListener('click', closeGroupModal);
createGroupConfirm.addEventListener('click', createGroup);
if (recoverHistoryBtn) {
    recoverHistoryBtn.addEventListener('click', startHistoryRecovery);
}
createGroupModal.addEventListener('click', (event) => {
    if (event.target === createGroupModal) {
        closeGroupModal();
    }
});
settingsBtn.addEventListener('click', openSettings);
settingsCancel.addEventListener('click', closeSettings);
settingsSave.addEventListener('click', () => saveSettings(false));
settingsSaveReboot.addEventListener('click', () => saveSettings(true));
settingsOpenNetwork.addEventListener('change', () => {
    settingsWifiPassword.disabled = settingsOpenNetwork.checked;
    if (settingsOpenNetwork.checked) {
        settingsWifiPassword.value = '';
    }
});
settingsModal.addEventListener('click', (event) => {
    if (event.target === settingsModal) {
        closeSettings();
    }
});

themeSwitch.addEventListener('change', (event) => {
    setTheme(event.target.checked);
});

const savedTheme = localStorage.getItem(STORAGE.theme);
setTheme(savedTheme === 'light' ? false : true);
chatNickname.textContent = getNickname();
renderMessages();
renderConversationList();
updateRecoveryControls();
