import { useHistory } from "../hooks/useHistory";

function formatTimestamp(ts?: { seconds: number } | null) {
  if (!ts) return "unknown";
  return new Date(ts.seconds * 1000).toLocaleString();
}

export function HistoryLog() {
  const { history, loading } = useHistory();

  return (
    <div className="panel">
      <h2>Feed history</h2>
      {loading && <p className="empty-state">Loading history.</p>}
      {!loading && history.length === 0 && (
        <p className="empty-state">No feed events recorded yet.</p>
      )}
      {history.length > 0 && (
        <table>
          <thead>
            <tr>
              <th>Time</th>
              <th>Quantity</th>
              <th>Source</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody>
            {history.map((item) => (
              <tr key={item.id}>
                <td>{formatTimestamp(item.timestamp)}</td>
                <td>{item.quantity ?? 1}</td>
                <td>{item.source ?? "unknown"}</td>
                <td>{item.status ?? "unknown"}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}
