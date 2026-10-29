import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('a full recovered cache still retains live messages before advancing the cursor', () => {
    const recovered = Array.from({ length: 2000 }, (_, index) => message(index + 1, { recovered: true }));
    const app = client({ 'esp-chat-messages': JSON.stringify(recovered) });
    const socket = connect(app);
    socket.receive(presence);
    socket.receive(message(2001));
    assert.equal(app.evaluate('allMessages.some((msg) => msg.id === 2001)'), true);
    assert.equal(app.evaluate('allMessages.length'), 2000);
    const reloaded = client(Object.fromEntries(app.storage));
    assert.equal(reloaded.evaluate('allMessages.some((msg) => msg.id === 2001)'), true);
});
