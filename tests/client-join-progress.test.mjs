import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('heartbeats alone cannot indefinitely postpone join completion', () => {
    const app = client();
    const socket = connect(app);
    app.tick(10000);
    socket.receive({ type: 'ping' });
    app.tick(5000);
    assert.equal(socket.readyState, 3);
});

test('valid replay progress renews the join timeout', () => {
    const app = client();
    const socket = connect(app);
    app.tick(10000);
    socket.receive(message(1));
    app.tick(10000);
    socket.receive(presence);
    assert.equal(socket.readyState, 1);
    assert.equal(app.evaluate('wsJoined'), true);
});
