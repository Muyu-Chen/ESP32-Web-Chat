import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('a half-open joined socket reconnects after the advertised heartbeat window', () => {
    const app = client();
    const socket = connect(app);
    socket.receive({ type: 'historyInfo', restore_before_id: 1, heartbeat_interval_s: 5 });
    socket.receive(presence);
    app.tick(14999);
    assert.equal(socket.readyState, 1);
    app.tick(1001);
    assert.equal(socket.readyState, 3);
    assert.equal(app.sockets.length, 2);
});

test('server heartbeat traffic renews the receive watchdog', () => {
    const app = client();
    const socket = connect(app);
    socket.receive({ type: 'historyInfo', restore_before_id: 1, heartbeat_interval_s: 5 });
    socket.receive(presence);
    for (let i = 0; i < 5; i++) {
        app.tick(10000);
        socket.receive({ type: 'ping' });
        assert.equal(socket.readyState, 1);
    }
    assert.equal(socket.sent.filter((msg) => msg.type === 'pong').length, 5);
});

test('invalid heartbeat intervals keep a conservative legacy timeout', () => {
    const app = client();
    const socket = connect(app);
    socket.receive({ type: 'historyInfo', restore_before_id: 1, heartbeat_interval_s: -10 });
    socket.receive(presence);
    app.tick(65000);
    assert.equal(socket.readyState, 1);
});

test('a successor resets the heartbeat interval advertised by its predecessor', () => {
    const app = client();
    const oldSocket = connect(app);
    oldSocket.receive({ type: 'historyInfo', restore_before_id: 1, heartbeat_interval_s: 5 });
    oldSocket.receive(presence);
    oldSocket.close();
    app.tick(1000);
    const socket = app.sockets[1];
    socket.open();
    socket.receive(presence);
    app.tick(65000);
    assert.equal(socket.readyState, 1);
});
test('late events from a retired socket cannot cancel the successor watchdog', () => {
    const app = client({}, { deferClose: true });
    const oldSocket = connect(app);
    app.tick(16000);
    const socket = app.sockets[1];
    socket.open();
    socket.receive({ type: 'historyInfo', restore_before_id: 1, heartbeat_interval_s: 5 });
    socket.receive(presence);
    oldSocket.receive(message(999));
    oldSocket.finishClose();
    assert.equal(app.evaluate('lastSeenId'), 0);
    app.tick(15000);
    assert.equal(socket.readyState, 2);
});
