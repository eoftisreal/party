import { Client, Databases, Account, Query, Permission, Role } from 'node-appwrite';
import { setTimeout as sleep } from 'node:timers/promises';
import { appwriteOperation } from './diagnostics.js';

// Appwrite 2.4.0 validates transaction TTL in [60, 3600] seconds.
const TRANSACTION_TTL_SECONDS = 60;

export function makeAppwriteStore(env, runtimeKey, { wait = sleep, random = Math.random } = {}) {
  const endpoint = env.APPWRITE_FUNCTION_API_ENDPOINT || env.APPWRITE_ENDPOINT;
  const project = env.APPWRITE_FUNCTION_PROJECT_ID || env.APPWRITE_PROJECT_ID;
  const databaseId = env.PARTY_DATABASE_ID;
  const key = env.PARTY_APPWRITE_API_KEY || runtimeKey;
  if (!endpoint || !project || !databaseId || !key) throw Error('Missing Appwrite configuration');
  const db = new Databases(new Client().setEndpoint(endpoint).setProject(project).setKey(key));
  const collections = Object.fromEntries(['rooms', 'members', 'peers', 'signals', 'notifications', 'limits'].map(name =>
    [name, env[`PARTY_${name.toUpperCase()}_COLLECTION_ID`] || name]));
  const maybe = async promise => { try { return await promise; } catch (e) { if (e.code === 404) return null; throw e; } };
  function scoped(transactionId) {
    const args = name => ({ databaseId, collectionId: collections[name], ...(transactionId ? { transactionId } : {}) });
    const get = (name, id) => maybe(appwriteOperation(`documents.get:${name}`, () => db.getDocument({ ...args(name), documentId: id })));
    const create = (name, id, data, permissions = []) => appwriteOperation(`documents.create:${name}`, () => db.createDocument({ ...args(name), documentId: id, data, permissions }));
    const update = (name, id, data) => appwriteOperation(`documents.update:${name}`, () => db.updateDocument({ ...args(name), documentId: id, data }));
    const remove = (name, id) => appwriteOperation(`documents.delete:${name}`, () => db.deleteDocument({ ...args(name), documentId: id }));
    async function list(name, filters) {
      const documents = [];
      let cursor;
      do {
        const page = await appwriteOperation(`documents.list:${name}`, () => db.listDocuments({ ...args(name), queries: [...filters, Query.limit(100), ...(cursor ? [Query.cursorAfter(cursor)] : [])] }));
        documents.push(...page.documents);
        if (page.documents.length < 100) break;
        cursor = page.documents.at(-1).$id;
      } while (true);
      return documents;
    }
    return {
      isTransaction: Boolean(transactionId),
      getRoom: id => get('rooms', id), createRoom: (id, data) => create('rooms', id, data),
      updateRoom: (id, data) => update('rooms', id, data), deleteRoom: id => remove('rooms', id),
      bumpRoom: id => appwriteOperation('documents.update:rooms', () => db.incrementDocumentAttribute({ ...args('rooms'), documentId: id, attribute: 'revision', value: 1 })),
      getMember: id => get('members', id), createMember: (id, data) => create('members', id, data),
      updateMember: (id, data) => update('members', id, data), deleteMember: id => remove('members', id),
      listMembers: id => list('members', [Query.equal('roomId', id)]),
      getPeer: id => get('peers', id), createPeer: (id, data) => create('peers', id, data),
      updatePeer: (id, data) => update('peers', id, data), deletePeer: id => remove('peers', id),
      listPeers: id => list('peers', [Query.equal('roomId', id)]),
      getSignal: id => get('signals', id), createSignal: (id, data) => create('signals', id, data),
      deleteSignal: id => remove('signals', id),
      listSignals: id => list('signals', [Query.equal('roomId', id)]),
      getReceipt: id => get('notifications', id),
      getLimit: id => get('limits', id), createLimit: (id,data) => create('limits',id,data), updateLimit: (id,data) => update('limits',id,data),
      notify: async (id, data) => {
        if (!await get('notifications', id)) return create('notifications', id, data, data.userId.startsWith('g_') ? [] : [Permission.read(Role.user(data.userId))]);
      },
      listNotifications: () => list('notifications', []), deleteNotification: id => remove('notifications', id),
      listRooms: () => list('rooms', []),
    };
  }
  const root = scoped();
  root.atomic = async work => {
    for (let attempt = 0; attempt < 4; ++attempt) {
      const tx = await appwriteOperation('transactions.create', () => db.createTransaction({ ttl: TRANSACTION_TTL_SECONDS }));
      try {
        const result = await work(scoped(tx.$id));
        await appwriteOperation('transactions.commit', () => db.updateTransaction({ transactionId: tx.$id, commit: true }));
        return result;
      } catch (e) {
        try { await appwriteOperation('transactions.rollback', () => db.updateTransaction({ transactionId: tx.$id, rollback: true })); } catch { /* preserve original error */ }
        if (e.code !== 409 || attempt === 3) throw e;
        // Independent Functions must not retry in lockstep against the same room.
        // Roll back first, then replay the entire operation in a fresh transaction.
        await wait(Math.floor((25 + random() * 75) * 2 ** attempt));
      }
    }
  };
  return root;
}
export async function verifyUserJWT(token, env) {
  const endpoint = env.APPWRITE_FUNCTION_API_ENDPOINT || env.APPWRITE_ENDPOINT;
  const project = env.APPWRITE_FUNCTION_PROJECT_ID || env.APPWRITE_PROJECT_ID;
  if (!endpoint || !project) throw Error('Missing project info');
  return (await appwriteOperation('account.get', () => new Account(new Client().setEndpoint(endpoint).setProject(project).setJWT(token)).get())).$id;
}
