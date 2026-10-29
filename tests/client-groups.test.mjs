import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('recovered group text keeps its conversation metadata', () => {
    const app = client();
    const groupMessage = message(5, {
        groupId: 'group-a', groupName: 'Alice and Bob', to: { all: false, users: ['alice', 'bob'] }
    });
    const payload = app.evaluate(`historyMessagePayload(${JSON.stringify(groupMessage)})`);
    assert.equal(payload.groupId, 'group-a');
    assert.equal(payload.groupName, 'Alice and Bob');
    app.evaluate('historyInfo = { restore_before_id: 100 }');
    assert.equal(app.evaluate(`importRecoveredMessage(${JSON.stringify(payload)}, { from: 'bob', name: 'Bob' })`), 'imported');
    assert.equal(app.evaluate("conversations['group-a'].type"), 'group');
});

test('invalid group metadata is rejected without advancing the cursor', () => {
    const app = client();
    const socket = connect(app);
    socket.receive(presence);
    socket.receive(message(99, { groupId: {}, groupName: 'Invalid' }));
    assert.equal(app.evaluate('lastSeenId'), 0);
    assert.equal(app.evaluate('allMessages.length'), 0);
});
