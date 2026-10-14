import {makeRoomsService} from '../src/rooms.js';
import {makeSignalingService} from '../src/signaling.js';
export function fixture(options = {}) {
  const tables = Object.fromEntries(['Room','Member','Peer','Signal','Notification','Limit'].map(n => [n,new Map()]));
  let time = Date.parse('2026-10-13T12:00:00Z'), serial = Promise.resolve();
  function methods(t) {
    const store = {};
    for (const [name, map] of Object.entries(t)) {
      store[`get${name}`] = async id => structuredClone(map.get(id) || null);
      store[`create${name}`] = async (id, data) => {
        if (map.has(id)) throw Object.assign(Error('duplicate'), {code:409});
        const document = {$id:id,...structuredClone(data)}; map.set(id,document); return structuredClone(document);
      };
      store[`update${name}`] = async (id,data) => {
        if (!map.has(id)) throw Object.assign(Error('missing'), {code:404});
        const value = {...map.get(id),...structuredClone(data)}; map.set(id,value); return structuredClone(value);
      };
      store[`delete${name}`] = async id => { map.delete(id); };
    }
    store.listMembers = async id => [...t.Member.values()].filter(x=>x.roomId===id).map(x=>structuredClone(x));
    store.listPeers = async id => [...t.Peer.values()].filter(x=>x.roomId===id).map(x=>structuredClone(x));
    store.listSignals = async id => [...t.Signal.values()].filter(x=>x.roomId===id).map(x=>structuredClone(x));
    store.listRooms = async () => [...t.Room.values()].map(x=>structuredClone(x));
    store.listNotifications = async () => [...t.Notification.values()].map(x=>structuredClone(x));
    store.getReceipt = store.getNotification;
    store.notify = (id,data) => store.createNotification(id,data);
    return store;
  }
  const store = methods(tables);
  store.atomic = work => {
    const operation = serial.then(async () => {
      const copy = Object.fromEntries(Object.entries(tables).map(([name,map])=>[name,new Map([...map].map(([id,data])=>[id,structuredClone(data)]))]));
      const scoped = methods(copy); scoped.isTransaction = true;
      const result = await work(scoped);
      for (const [name,map] of Object.entries(copy)) { tables[name].clear(); for (const [id,data] of map) tables[name].set(id,data); }
      return result;
    });
    serial = operation.catch(()=>{}); return operation;
  };
  const rooms = makeRoomsService(store,{...options,now:()=>time});
  return {store,rooms,signaling:makeSignalingService(store,rooms),rs:tables.Room,ms:tables.Member,tables,advance:ms=>time+=ms};
}
