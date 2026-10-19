// Never serialize SDK exceptions: message/response/stack can contain request secrets.
const operations = new WeakMap();
const names = new Set(['Error', 'TypeError', 'RangeError', 'SyntaxError', 'AppwriteException']);
const types = new Set([
  'general_argument_invalid', 'general_unauthorized_scope', 'general_unauthorized',
  'general_unknown', 'general_server_error', 'general_rate_limit_exceeded',
  'general_route_not_found', 'general_protocol_unsupported',
  'database_not_found', 'collection_not_found', 'document_not_found',
  'document_invalid_structure', 'document_already_exists', 'document_update_conflict',
  'transaction_not_found', 'transaction_not_ready', 'transaction_expired',
  'transaction_invalid', 'transaction_conflict', 'transaction_limit_exceeded',
]);
const collections = new Set(['rooms', 'members', 'peers', 'signals', 'notifications', 'limits']);
function validOperation(operation) {
  if (['transactions.create', 'transactions.commit', 'transactions.rollback', 'configuration.load', 'request.process'].includes(operation)) return true;
  const [action, collection] = operation.split(':');
  return ['documents.get', 'documents.create', 'documents.update', 'documents.delete', 'documents.list'].includes(action) && collections.has(collection);
}
export async function appwriteOperation(operation, work) {
  if (!validOperation(operation)) throw new TypeError('Invalid diagnostic operation');
  try { return await work(); }
  catch (error) {
    if (error && typeof error === 'object' && !operations.has(error)) operations.set(error, operation);
    throw error; // Preserve SDK status and identity for conflict retries and auth handling.
  }
}
export function safeDiagnostic(error, fallback = 'request.process') {
  return {
    event: 'party_backend_error',
    operation: operations.get(error) || (validOperation(fallback) ? fallback : 'unknown'),
    status: Number.isInteger(error?.code) && error.code >= 100 && error.code <= 599 ? error.code : null,
    errorType: types.has(error?.type) ? error.type : 'unknown',
    errorName: names.has(error?.name) ? error.name : 'Error',
  };
}
export function logDiagnostic(log, error, fallback) {
  try { log?.(JSON.stringify(safeDiagnostic(error, fallback))); } catch { /* logging must not change API behavior */ }
}
