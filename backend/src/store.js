import { Client, Databases, Account, Query, Permission, Role } from 'node-appwrite';

export function makeAppwriteStore(env, runtimeKey) {
  const endpoint = env.APPWRITE_FUNCTION_API_ENDPOINT || env.APPWRITE_ENDPOINT;
  const project = env.APPWRITE_FUNCTION_PROJECT_ID || env.APPWRITE_PROJECT_ID;
  const databaseId = env.PARTY_DATABASE_ID;
  const key = env.PARTY_APPWRITE_API_KEY || runtimeKey;
  if (!endpoint || !project || !databaseId || !key) throw Error('Missing Appwrite configuration');
  const db = new Databases(new Client().setEndpoint(endpoint).setProject(project).setKey(key));
  const collections = Object.fromEntries(['rooms', 'members', 'peers', 'signals', 'notifications'].map(name =>
    [name, env[`PARTY_${name.toUpperCase()}_COLLECTION_ID`] || name]));
  const maybe = async promise => { try { return await promise; } catch (e) { if (e.code === 404) return null; throw e; } };
  function scoped(transactionId) {
    const args = name => ({ databaseId, collectionId: collections[name], ...(transactionId ? { transactionId } : {}) });
    const get = (name, id) => maybe(db.getDocument({ ...args(name), documentId: id }));
    const create = (name, id, data, permissions = []) => db.createDocument({ ...args(name), documentId: id, data, permissions });
    const update = (name, id, data) => db.updateDocument({ ...args(name), documentId: id, data });
    const remove = (name, id) => db.deleteDocument({ ...args(name), documentId: id });
    async function list(name, filters) {
      const documents = [];
      let cursor;
      do {
        const page = await db.listDocuments({ ...args(name), queries: [...filters, Query.limit(100), ...(cursor ? [Query.cursorAfter(cursor)] : [])] });
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
      getMember: id => get('members', id), createMember: (id, data) => create('members', id, data),
      updateMember: (id, data) => update('members', id, data), deleteMember: id => remove('members', id),
      listMembers: id => list('members', [Query.equal('roomId', id)]),
      getPeer: id => get('peers', id), createPeer: (id, data) => create('peers', id, data),
      updatePeer: (id, data) => update('peers', id, data),
      listPeers: id => list('peers', [Query.equal('roomId', id)]),
      getSignal: id => get('signals', id), createSignal: (id, data) => create('signals', id, data),
      deleteSignal: id => remove('signals', id),
      listSignals: id => list('signals', [Query.equal('roomId', id)]),
      notify: async (id, data) => {
        if (!await get('notifications', id)) return create('notifications', id, data, [Permission.read(Role.user(data.userId))]);
      },
      listNotifications: () => list('notifications', []), deleteNotification: id => remove('notifications', id),
      listRooms: () => list('rooms', []),
    };
  }
  const root = scoped();
  root.atomic = async work => {
    for (let attempt = 0; attempt < 4; ++attempt) {
      const tx = await db.createTransaction({ ttl: 30 });
      try {
        const result = await work(scoped(tx.$id));
        await db.updateTransaction({ transactionId: tx.$id, commit: true });
        return result;
      } catch (e) {
        try { await db.updateTransaction({ transactionId: tx.$id, rollback: true }); } catch { /* preserve original error */ }
        if (e.code !== 409 || attempt === 3) throw e;
      }
    }
  };
  return root;
}
export async function verifyUserJWT(token, env) {
  const endpoint = env.APPWRITE_FUNCTION_API_ENDPOINT || env.APPWRITE_ENDPOINT;
  const project = env.APPWRITE_FUNCTION_PROJECT_ID || env.APPWRITE_PROJECT_ID;
  if (!endpoint || !project) throw Error('Missing project info');
  return (await new Account(new Client().setEndpoint(endpoint).setProject(project).setJWT(token)).get()).$id;
}
