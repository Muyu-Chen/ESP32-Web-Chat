import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('a stuck handshake falls back and stale events cannot replace its successor', () => {
    const app = client();
    app.evaluate('connect()');
    const first = app.sockets[0];
    app.tick(10250);
    assert.equal(first.readyState, 3);
    assert.equal(app.sockets.length, 2);
    const second = app.sockets[1];
    assert.equal(second.url, 'ws://192.168.4.1/ws');
    first.onopen();
    first.onclose({ code: 1006 });
    second.open();
    second.receive(presence);
    app.tick(20000);
    assert.equal(second.readyState, 1);
    assert.equal(app.evaluate('wsJoined'), true);
});

test('join confirmation gates the outbox and failed sends retain unsent messages', () => {
    const app = client();
    app.evaluate(`outbox = [${JSON.stringify(message(1, { from: 'alice' }))}]`);
    const socket = connect(app);
    assert.deepEqual(socket.sent.map((msg) => msg.type), ['join']);
    socket.receive({ type: 'onlineUsers', data: [] });
    assert.equal(app.evaluate('wsJoined'), false);
    socket.receive(presence);
    assert.deepEqual(socket.sent.map((msg) => msg.type), ['join', 'text']);
    assert.equal(app.evaluate('outbox.length'), 0);
    app.evaluate(`outbox = [${JSON.stringify(message(2, { from: 'alice' }))}]`);
    socket.failSend = true;
    app.evaluate('flushOutbox()');
    assert.equal(app.evaluate('outbox.length'), 1);
});

test('a socket without join progress times out and reconnects', () => {
    const app = client();
    const socket = connect(app);
    app.tick(15000);
    assert.equal(socket.readyState, 3);
    app.tick(1000);
    assert.equal(app.sockets.length, 2);
});

test('bad_since_id clears the cursor and session_replaced suppresses reconnect', () => {
    const app = client({ 'esp-chat-last-seen-id': '100' });
    let socket = connect(app);
    socket.receive({ type: 'error', code: 'bad_since_id' });
    assert.equal(app.storage.get('esp-chat-last-seen-id'), '0');
    app.tick(1000);
    socket = app.sockets.at(-1);
    socket.open();
    assert.equal(socket.sent[0].since_id, 0);
    socket.receive(presence);
    socket.receive({ type: 'error', code: 'session_replaced' });
    socket.close();
    app.tick(60000);
    app.listeners.get('online')();
    assert.equal(app.sockets.length, 2);
    assert.equal(app.evaluate('reconnectBlocked'), true);
});
