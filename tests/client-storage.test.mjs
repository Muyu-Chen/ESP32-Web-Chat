import assert from 'node:assert/strict';
import test from 'node:test';
import { client, message, connect, presence } from './client-helpers.mjs';

test('wrong-shaped local JSON does not prevent startup', () => {
    for (const value of ['null', '{}', '42', '"text"', '[null]']) {
        const app = client({
            'esp-chat-messages': value,
            'esp-chat-conversations': value,
            'esp-chat-outbox': value,
            'esp-chat-last-seen-id': 'NaN'
        });
        assert.equal(app.evaluate('allMessages.length'), 0);
        assert.equal(app.evaluate('outbox.length'), 0);
        assert.equal(app.evaluate('lastSeenId'), 0);
    }
});

test('denied local storage still permits joining and receiving chat', () => {
    const app = client({}, { denied: true });
    const socket = connect(app);
    socket.receive({ type: 'onlineUsers', data: [{ id: app.evaluate('userId'), name: 'Alice' }] });
    socket.receive(message());
    assert.equal(app.evaluate('wsJoined'), true);
    assert.equal(app.evaluate('allMessages.length'), 1);
});

test('stored messages restore conversations when the conversation cache is corrupt', () => {
    const app = client({
        'esp-chat-messages': JSON.stringify([message(3, { to: { all: false, users: ['alice'] } })]),
        'esp-chat-conversations': 'null'
    });
    assert.equal(app.evaluate('conversations.bob.name'), 'Bob');
    assert.equal(app.evaluate('conversations.bob.type'), 'private');
    assert.equal(app.evaluate('lastSeenId'), 3);
});

test('a malformed cached global conversation cannot turn the public chat into a private one', () => {
    const app = client({
        'esp-chat-conversations': JSON.stringify({
            global: { id: 'global', name: 'Wrong', type: 'private', members: [] }
        })
    });
    assert.equal(app.evaluate('currentConversation.type'), 'global');
    assert.equal(app.evaluate('currentConversation.id'), 'global');
});
