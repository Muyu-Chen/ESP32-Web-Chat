import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('control messages and invalid chat cannot poison the replay cursor', () => {
    const app = client();
    const socket = connect(app);
    socket.receive({ ...presence, id: 999999 });
    socket.receive(message(999999, { data: {} }));
    assert.equal(app.evaluate('lastSeenId'), 0);
    socket.receive(message(4));
    socket.receive(message(4));
    assert.equal(app.evaluate('lastSeenId'), 4);
    assert.equal(app.evaluate('allMessages.length'), 1);
    socket.receive(message(5, { to: { all: false, users: ['carol'] } }));
    assert.equal(app.evaluate('lastSeenId'), 5);
    assert.equal(app.evaluate('allMessages.length'), 1);
});

test('failed message persistence cannot advance the durable replay cursor', () => {
    const app = client();
    const socket = connect(app);
    socket.receive(presence);
    app.options.failKey = 'esp-chat-messages';
    socket.receive(message(8));
    socket.receive(message(9, { to: { all: false, users: ['carol'] } }));
    assert.equal(app.storage.get('esp-chat-last-seen-id'), '0');
    app.options.failKey = null;
    socket.receive(message(8));
    assert.equal(app.evaluate('allMessages.length'), 1);
    const reloaded = client(Object.fromEntries(app.storage));
    assert.equal(reloaded.evaluate('allMessages.length'), 1);
    assert.equal(reloaded.evaluate('lastSeenId'), 8);
});
