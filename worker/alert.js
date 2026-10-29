export const isFresh = (d, now = Date.now(), staleMs = 120000) => !!d && now - d.ts <= staleMs;

// Returns the list of alert lines for a /live reading (empty = all fine or stale).
export function alertMessages(d, now = Date.now(), staleMs = 120000) {
  if (!isFresh(d, now, staleMs)) return [];
  const m = [];
  if (d.tempStatus === "FEVER") m.push(`High temperature: ${Number(d.temperature).toFixed(1)} °C`);
  if (d.bpmStatus === "HIGH") m.push(`High heart rate: ${d.bpm} BPM`);
  if (d.bpmStatus === "LOW") m.push(`Low heart rate: ${d.bpm} BPM`);
  return m;
}
