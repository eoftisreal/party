import test from 'node:test';
import assert from 'node:assert/strict';
import { makeHandler } from '../src/http.js';
import { safeDiagnostic, appwriteOperation } from '../src/diagnostics.js';

const secret = 'NEVER_LOG_TOKEN_KEY_INVITE_BODY';
test('unknown startup errors identify their request stage and backend callsite without secrets', async () => {
  for (const operation of ['request.body', 'request.ice', 'request.create', 'request.join', 'request.response']) {
    const logs = [];
    let response;
    const fail = () => { throw Error(secret); };
    const joining = operation === 'request.join';
    const req = { method: 'POST', path: '/v1/rooms' + (joining ? '/' + 'a'.repeat(32) + '/join' : ''),
      headers: {}, bodyJson: { title: 'Movie' } };
    if (operation === 'request.body') Object.defineProperty(req, 'bodyText', { get: fail });
    const handler = makeHandler({
      service: {
        create: operation === 'request.create' ? fail : async () => ({ roomId: 'a'.repeat(32) }),
        publicLimit: async () => {},
        join: fail,
      },
      signaling: {},
      getIceSettings: operation === 'request.ice' ? fail : () => ({}),
      logError: (error, stage) => logs.push(safeDiagnostic(error, stage)),
    });
    await handler({ req, res: { json(body, status) {
      if (operation === 'request.response' && status < 300) fail();
      response = { body, status };
    } } });
    assert.equal(response.status, 500);
    assert.equal(response.body.error.code, 'INTERNAL_ERROR');
    assert.equal(logs.length, 1);
    assert.equal(logs[0].operation, operation);
    assert.equal(logs[0].location.file, 'http.js');
    assert.ok(logs[0].location.line > 0);
    assert.ok(!JSON.stringify({ response, logs }).includes(secret));
  }
});

test('location output contains only an allowed source filename and bounded numbers', () => {
  const error = Error(secret);
  error.stack = `Error: ${secret}\n    at secretFunction (/private/${secret}/backend/src/rooms.js:89:21)`;
  assert.deepEqual(safeDiagnostic(error).location, { file: 'rooms.js', line: 89, column: 21 });
  assert.ok(!JSON.stringify(safeDiagnostic(error)).includes(secret));
  assert.ok(!JSON.stringify(safeDiagnostic(error)).includes('private'));
  error.stack = `Error: ${secret}\n    at C:\\private\\backend\\src\\store.js:12:3`;
  assert.deepEqual(safeDiagnostic(error).location, { file: 'store.js', line: 12, column: 3 });
  for (const frame of ['unknown.js:12:3', 'rooms.js:1000000:3', 'rooms.js:12:3?token=secret']) {
    error.stack = `Error: ${secret}\n    at /private/backend/src/${frame}`;
    assert.ok(!('location' in safeDiagnostic(error)));
  }
});

test('mapped SDK operations retain their existing diagnostic shape and precedence', async () => {
  const error = Object.assign(Error(secret), { code: 403, name: 'AppwriteException', type: 'general_unauthorized_scope' });
  error.stack = 'Error\n    at /backend/src/store.js:12:3';
  await assert.rejects(appwriteOperation('documents.create:rooms', () => Promise.reject(error)));
  assert.deepEqual(safeDiagnostic(error, 'request.create'), {
    event: 'party_backend_error', operation: 'documents.create:rooms', status: 403,
    errorType: 'general_unauthorized_scope', errorName: 'AppwriteException',
  });
});
