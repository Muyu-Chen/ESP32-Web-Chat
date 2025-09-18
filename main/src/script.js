'''const loginContainer = document.getElementById('login-container');
const chatContainer = document.getElementById('chat-container');
const joinBtn = document.getElementById('join-btn');
const nicknameInput = document.getElementById('nickname');
const chatNickname = document.getElementById('chat-nickname');
const themeSwitch = document.getElementById('checkbox');

const messages = document.getElementById('messages');
const form = document.getElementById('form');
const input = document.getElementById('message-input');

let ws;
function generateUUID() { // Public Domain/MIT
    var d = new Date().getTime();//Timestamp
    var d2 = ((typeof performance !== 'undefined') && performance.now && (performance.now()*1000)) || 0;//Time in microseconds since page-load or 0 if unsupported
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
        var r = Math.random() * 16;//random number between 0 and 16
        if(d > 0){//Use timestamp until depleted
            r = (d + r)%16 | 0;
            d = Math.floor(d/16);
        } else {//Use microseconds since page-load if supported
            r = (d2 + r)%16 | 0;
            d2 = Math.floor(d2/16);
        }
        return (c === 'x' ? r : (r & 0x3 | 0x8)).toString(16);
    });
}

let userId = localStorage.getItem('esp-chat-uuid');
if (!userId) {
    userId = generateUUID();
    localStorage.setItem('esp-chat-uuid', userId);
}

nicknameInput.value = localStorage.getItem('esp-chat-nickname') || `User${Math.floor(Math.random() * 1000)}`;

joinBtn.addEventListener('click', () => {
    const nickname = nicknameInput.value.trim();
    if (nickname) {
        localStorage.setItem('esp-chat-nickname', nickname);
        chatNickname.textContent = `Logged in as: ${nickname}`;
        
        loginContainer.style.opacity = '0';
        setTimeout(() => {
            loginContainer.style.display = 'none';
            chatContainer.classList.add('visible');
            connect();
        }, 500); // Match CSS transition duration

    } else {
        alert('Please enter a nickname.');
    }
});

function showMessage(msg) {
    const item = document.createElement('div');
    item.classList.add('message');
    
    const from = document.createElement('div');
    from.classList.add('from');
    from.textContent = msg.name;

    const data = document.createElement('div');
    data.textContent = msg.data;

    const timestamp = document.createElement('span');
    timestamp.classList.add('timestamp');
    timestamp.textContent = new Date(msg.timestamp * 1000).toLocaleTimeString();

    if (msg.from === userId) {
        item.classList.add('mine');
    }

    item.appendChild(from);
    item.appendChild(data);
    item.appendChild(timestamp);
    messages.prepend(item);
}

function showSystemMessage(text) {
    const item = document.createElement('div');
    item.classList.add('system');
    item.textContent = text;
    messages.prepend(item);
}

function connect() {
    const host = window.location.hostname;
    console.log('Attempting to connect to WebSocket at', `ws://${host}/ws`);
    ws = new WebSocket(`ws://${host}/ws`);

    ws.onopen = () => {
        console.log('WebSocket connection opened.');
        showSystemMessage('Connected to the server.');
    };

    ws.onmessage = (event) => {
        console.log('Received raw message:', event.data);
        try {
            const msg = JSON.parse(event.data);

            if (msg.type === 'ping') {
                ws.send('{"type":"pong"}');
                return;
            }

            console.log('Parsed message:', msg);
            if (msg.to && msg.to.users && !msg.to.all) {
                if (msg.to.users.includes(userId) || msg.from === userId) {
                    showMessage(msg);
                }
            } else {
                showMessage(msg);
            }
        } catch (e) {
            console.error('Error parsing message JSON:', e);
        }
    };

    ws.onclose = (event) => {
        console.log('WebSocket connection closed. Code:', event.code, 'Reason:', event.reason, 'wasClean:', event.wasClean);
        showSystemMessage('Disconnected. Trying to reconnect in 3 seconds...');
        setTimeout(connect, 3000);
    };

    ws.onerror = (error) => {
        console.error('WebSocket Error:', error);
    };
}

function sendMessage(event) {
    event.preventDefault();
    if (input.value && ws && ws.readyState === WebSocket.OPEN) {
        const msg = {
            type: 'text',
            from: userId,
            to: { all: true, users: [] },
            name: localStorage.getItem('esp-chat-nickname'),
            data: input.value,
            id: 0, // Server will assign
            timestamp: Math.floor(Date.now() / 1000)
        };
        const jsonMsg = JSON.stringify(msg);
        console.log('Sending message:', jsonMsg);
        setTimeout(() => {
            ws.send(jsonMsg);
        }, 0);
        input.value = '';
    } else {
        console.log('Could not send message. WebSocket not open. ReadyState:', ws ? ws.readyState : 'ws is null');
    }
}

function setTheme(isDark) {
    if (isDark) {
        document.body.classList.add('dark-mode');
        themeSwitch.checked = true;
        localStorage.setItem('theme', 'dark');
    } else {
        document.body.classList.remove('dark-mode');
        themeSwitch.checked = false;
        localStorage.setItem('theme', 'light');
    }
}

themeSwitch.addEventListener('change', (e) => {
    setTheme(e.target.checked);
});

// Apply saved theme on load
const savedTheme = localStorage.getItem('theme');
// Default to dark mode if no theme is saved
setTheme(savedTheme === 'light' ? false : true);
'''