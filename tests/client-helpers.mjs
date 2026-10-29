import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source = readFileSync(process.env.CHAT_CLIENT_SCRIPT || new URL('../main/web/js/script.js', import.meta.url), 'utf8');

export function client(initialStorage = {}, options = {}) {
    const storage = new Map(Object.entries({
        'esp-chat-uuid': 'alice',
        'esp-chat-nickname': 'Alice',
        ...initialStorage
    }));
    const timers = new Map();
    const listeners = new Map();
    let nextTimer = 0;
    let now = 0;
    const elements = new Map();
    function element() {
        return {
            value: '', style: {}, dataset: {}, children: [],
            classList: { add() {}, remove() {}, toggle() {} },
            appendChild(child) { this.children.push(child); },
            addEventListener() {},
            querySelectorAll() { return []; }
        };
    }
    class Socket {
        static CONNECTING = 0;
        static OPEN = 1;
        static CLOSING = 2;
        static CLOSED = 3;
        static instances = [];
        constructor(url) {
            this.url = url;
            this.readyState = Socket.CONNECTING;
            this.sent = [];
            Socket.instances.push(this);
        }
        open() { this.readyState = Socket.OPEN; this.onopen(); }
        send(encoded) {
            if (this.failSend) { throw new Error('send failed'); }
            this.sent.push(JSON.parse(encoded));
        }
        receive(message) { this.onmessage({ data: JSON.stringify(message) }); }
        close() {
            this.readyState = Socket.CLOSING;
            if (!options.deferClose) { this.finishClose(); }
        }
        finishClose() {
            this.readyState = Socket.CLOSED;
            this.onclose?.({ code: 1006, reason: '', wasClean: false });
        }
    }
    const context = vm.createContext({
        console: { log() {}, warn() {}, error() {}, debug() {} },
        WebSocket: Socket,
        document: {
            getElementById(id) {
                if (!elements.has(id)) { elements.set(id, element()); }
                return elements.get(id);
            },
            createElement: element,
            body: element()
        },
        window: {
            location: { protocol: 'http:', host: 'chat.test' },
            addEventListener(event, listener) { listeners.set(event, listener); }
        },
        localStorage: {
            getItem(key) {
                if (options.denied) { throw new Error('storage denied'); }
                return storage.get(key) ?? null;
            },
            setItem(key, value) {
                if (options.denied || options.failKey === key) { throw new Error('storage full'); }
                storage.set(key, String(value));
            }
        },
        setTimeout(callback, delay) {
            const id = ++nextTimer;
            timers.set(id, { callback, time: now + delay });
            return id;
        },
        clearTimeout(id) { timers.delete(id); },
        alert() {}
    });
    vm.runInContext(source, context);
    function evaluate(code) { return vm.runInContext(code, context); }
    function tick(duration) {
        const end = now + duration;
        while (true) {
            const next = [...timers].filter(([, timer]) => timer.time <= end)
                .sort((a, b) => a[1].time - b[1].time)[0];
            if (!next) { break; }
            const [id, timer] = next;
            timers.delete(id);
            now = timer.time;
            timer.callback();
        }
        now = end;
    }
    return { evaluate, tick, storage, options, listeners, sockets: Socket.instances };
}

export function message(id = 1, extra = {}) {
    return {
        type: 'text', from: 'bob', name: 'Bob', data: 'hello', id,
        timestamp: 1780000000, to: { all: true, users: [] }, ...extra
    };
}

export function connect(app) {
    app.evaluate('hasJoined = true; connect()');
    const socket = app.sockets.at(-1);
    socket.open();
    return socket;
}

export const presence = { type: 'onlineUsers', data: [{ id: 'alice', name: 'Alice' }] };
