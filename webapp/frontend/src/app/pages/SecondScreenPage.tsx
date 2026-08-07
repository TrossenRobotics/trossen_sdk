/**
 * Secondary screen — the fixed status display mounted on the robot.
 *
 * A different product from the main UI, not a breakpoint of it. It is read from
 * one to two metres away, often mid-task with hands full, by someone who is not
 * the operator. So it answers a small number of questions in large type and
 * offers exactly one control: stop.
 *
 * Mounted OUTSIDE the app Layout (like EmbeddedViewerPage) so it carries no nav
 * chrome — the display is bolted to a robot and has nowhere to navigate to.
 *
 * PORTRAIT: the panel is fixed vertically, so the layout is a single column
 * sized in viewport units and never assumes width > height.
 *
 * MODALITY-AGNOSTIC: runs unchanged on every system. `base` is absent on a
 * stationary or solo rig, and the base tiles are omitted rather than showing
 * zeros — a battery reading of 0% and "no battery fitted" must not look alike.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { apiGet, apiPost, describeError } from '@/lib/api';

/** Poll period. The server samples the base at 2 Hz; 1 Hz here is a status
 *  board read from across a room, and halves the request rate for free. */
const POLL_MS = 1000;

/** Consecutive poll failures before the link is declared down. One dropped
 *  request over WiFi is normal and must not make the screen flap. */
const LINK_DOWN_AFTER = 3;

type BaseTelemetry = {
  id?: string;
  connected?: boolean;
  ready?: boolean;
  e_stopped?: boolean;
  battery?: {
    percent?: number;
    voltage?: number;
    current?: number;
    temp?: number;
    charging_state?: number;
  };
  pose?: { x?: number; y?: number; theta?: number };
  has_fault?: boolean;
  has_critical_fault?: boolean;
  faults?: { description?: string; critical?: boolean }[];
  /** 0 = auto-stop disabled. */
  estop_battery_percent?: number;
  /** False until the first BMS frame lands; percent reads 0 until then. */
  battery_reading_valid?: boolean;
  battery_below_estop_threshold?: boolean;
};

type ActiveSession = {
  id: string;
  name: string;
  status: string;
  current_episode: number;
  num_episodes: number;
  system_name: string;
  dry_run: boolean;
};

type Snapshot = {
  storage: { total: number; used: number; free: number; dataset_count: number };
  active_session: ActiveSession | null;
  base: BaseTelemetry | null;
};

const CHARGING_LABEL: Record<number, string> = {
  0: 'STATIONARY',
  1: 'CHARGING',
  2: 'DISCHARGING',
};

function gib(bytes: number): string {
  return `${(bytes / 1024 ** 3).toFixed(0)} GB`;
}

/** Battery colour by charge. Encoded as colour AND text everywhere it is used,
 *  so the state survives being glanced at by someone colour-blind.
 *
 *  Keyed off the configured auto-stop threshold rather than fixed percentages,
 *  so the colour means "how close am I to the robot stopping itself" on this
 *  rig. Falls back to fixed bands when the auto-stop is disabled. */
function batteryTone(percent: number, threshold?: number): string {
  if (threshold && threshold > 0) {
    if (percent <= threshold) return '#ff4d4d';
    // Amber for the last 10 points of headroom above the trip line.
    if (percent <= threshold + 10) return '#ffb020';
    return '#3ddc97';
  }
  if (percent <= 15) return '#ff4d4d';
  if (percent <= 30) return '#ffb020';
  return '#3ddc97';
}

function Tile({
  label,
  children,
  tone,
}: {
  label: string;
  children: React.ReactNode;
  tone?: string;
}) {
  return (
    <div
      className="bg-surface border border-edge rounded-md px-5 py-4"
      style={tone ? { borderLeft: `4px solid ${tone}` } : undefined}
    >
      <div className="text-dim text-[13px] tracking-wide mb-2">{label}</div>
      {children}
    </div>
  );
}

export function SecondScreenPage() {
  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [linkDown, setLinkDown] = useState(false);
  const [estopBusy, setEstopBusy] = useState(false);
  const [estopNote, setEstopNote] = useState<string | null>(null);
  const failures = useRef(0);

  useEffect(() => {
    let cancelled = false;
    let timer: number | undefined;

    const tick = async () => {
      try {
        const data = await apiGet<Snapshot>('/api/second-screen');
        if (cancelled) return;
        setSnap(data);
        failures.current = 0;
        setLinkDown(false);
      } catch {
        if (cancelled) return;
        failures.current += 1;
        // Keep the last snapshot on screen rather than blanking it — stale
        // numbers under an explicit LINK DOWN banner are more useful than
        // nothing, and blanking looks like the robot died.
        if (failures.current >= LINK_DOWN_AFTER) setLinkDown(true);
      } finally {
        if (!cancelled) timer = window.setTimeout(tick, POLL_MS);
      }
    };
    tick();
    return () => {
      cancelled = true;
      if (timer) window.clearTimeout(timer);
    };
  }, []);

  const session = snap?.active_session ?? null;
  const base = snap?.base ?? null;

  // No confirmation dialog, deliberately: a stop you have to confirm is not a
  // stop. The tradeoff is that a stray touch ends the session — recoverable,
  // where a delayed stop may not be.
  const onEstop = useCallback(async () => {
    if (!session) return;
    setEstopBusy(true);
    setEstopNote(null);
    try {
      await apiPost(`/api/sessions/${session.id}/emergency-stop`);
      setEstopNote('STOP SENT — base halted, arms homing');
    } catch (err) {
      setEstopNote(describeError(err));
    } finally {
      setEstopBusy(false);
    }
  }, [session]);

  const percent = base?.battery?.percent;
  const estopped = base?.e_stopped === true;

  return (
    <div
      className="bg-app text-ink font-['JetBrains_Mono',monospace] flex flex-col"
      style={{ width: '100vw', minHeight: '100vh', padding: '3vh 4vw', gap: '2vh' }}
    >
      {/* Identity + link. Both always present: the screen must be readable as
          "which robot am I looking at, and is this data live". */}
      <header className="flex items-baseline justify-between border-b border-edge pb-3">
        <div>
          <div className="text-dim text-[13px] tracking-wide">SYSTEM</div>
          <div className="text-[clamp(20px,3.4vh,34px)] leading-tight">
            {session?.system_name ?? 'IDLE'}
          </div>
        </div>
        <div className="text-right">
          <div className="text-dim text-[13px] tracking-wide">LINK</div>
          <div
            className="text-[clamp(15px,2.2vh,22px)]"
            style={{ color: linkDown ? '#ff4d4d' : '#3ddc97' }}
          >
            {linkDown ? 'DOWN' : 'UP'}
          </div>
        </div>
      </header>

      {linkDown && (
        <div
          className="rounded-md px-4 py-3 text-[clamp(13px,1.8vh,17px)]"
          style={{ background: '#3a1111', border: '1px solid #ff4d4d' }}
        >
          LINK DOWN — values below are the last received and may be stale.
        </div>
      )}

      {/* E-stop state is the loudest thing on the screen when it is active,
          because someone walking up needs to know before anything else. */}
      {estopped && (
        <div
          className="rounded-md px-4 py-4 text-center text-[clamp(18px,3vh,30px)]"
          style={{ background: '#3a1111', border: '2px solid #ff4d4d' }}
        >
          BASE E-STOPPED
          <div className="text-dim text-[13px] mt-1 tracking-wide">
            SOFTWARE OR PHYSICAL — BOTH REPORT HERE
          </div>
        </div>
      )}

      {/* Battery: the headline number, largest thing on an otherwise calm
          screen. Absent (not zero) when the system has no base. */}
      {base ? (
        <Tile
          label="BATTERY"
          tone={
            base.battery_reading_valid && percent !== undefined
              ? batteryTone(percent, base.estop_battery_percent)
              : undefined
          }
        >
          <div className="flex items-baseline gap-4">
            <div
              className="text-[clamp(48px,11vh,120px)] leading-none"
              style={{
                color:
                  base.battery_reading_valid && percent !== undefined
                    ? batteryTone(percent, base.estop_battery_percent)
                    : undefined,
              }}
            >
              {/* A 0% reading is "no BMS frame yet", not a flat battery -- the
                  same distinction the auto-stop guard makes. Showing a dash
                  rather than 0% keeps the screen from implying the robot is
                  about to stop itself while it is merely still waking up. */}
              {base.battery_reading_valid && percent !== undefined
                ? `${percent.toFixed(0)}%`
                : '—'}
            </div>
            <div className="text-dim text-[clamp(12px,1.7vh,16px)] leading-snug">
              {base.battery?.voltage !== undefined && (
                <div>{base.battery.voltage.toFixed(1)} V</div>
              )}
              {base.battery?.temp !== undefined && (
                <div>{base.battery.temp.toFixed(0)} °C</div>
              )}
              {base.battery?.charging_state !== undefined && (
                <div>{CHARGING_LABEL[base.battery.charging_state] ?? '—'}</div>
              )}
            </div>
          </div>
          {/* State the trip line explicitly. An operator who can see the limit
              can plan around it; one who cannot just gets stopped mid-episode
              by a number nobody showed them. */}
          <div className="text-dim text-[clamp(11px,1.5vh,14px)] mt-2">
            {base.estop_battery_percent && base.estop_battery_percent > 0
              ? `Auto-stop at ${base.estop_battery_percent.toFixed(0)}%`
              : 'Auto-stop disabled'}
            {/* Two different reasons for a dash, and they need different
                responses. `telemetry()` returns only {id, connected} when the
                driver is absent, so `battery_reading_valid` is undefined rather
                than false -- indistinguishable from "no BMS frame yet" unless
                `connected` is checked first. Saying "waiting for first BMS
                reading" about a base that is not connected sends the operator
                off to wait for something that will never arrive. */}
            {base.connected === false
              ? ' · base not connected'
              : !base.battery_reading_valid && ' · waiting for first BMS reading'}
          </div>
        </Tile>
      ) : (
        <Tile label="BATTERY">
          <div className="text-dim text-[clamp(14px,2vh,18px)]">
            {session
              ? 'No mobile base on this system'
              : 'No session running — base telemetry unavailable'}
          </div>
        </Tile>
      )}

      {/* Odometry. Pose is measured; the base reports no velocity feedback, so
          nothing here claims to be a speed. */}
      {base?.pose && (
        <Tile label="ODOMETRY">
          <div className="grid grid-cols-3 gap-3 text-[clamp(18px,3.2vh,32px)]">
            {(['x', 'y', 'theta'] as const).map((k) => (
              <div key={k}>
                <div className="text-dim text-[12px] tracking-wide">
                  {k === 'theta' ? 'θ (rad)' : `${k.toUpperCase()} (m)`}
                </div>
                <div>{(base.pose?.[k] ?? 0).toFixed(2)}</div>
              </div>
            ))}
          </div>
        </Tile>
      )}

      {base?.has_fault && (
        <Tile label="FAULTS" tone={base.has_critical_fault ? '#ff4d4d' : '#ffb020'}>
          <div className="text-[clamp(13px,1.9vh,17px)] leading-snug">
            {(base.faults ?? []).map((f, i) => (
              <div key={i} style={{ color: f.critical ? '#ff4d4d' : '#ffb020' }}>
                {f.critical ? '[CRITICAL] ' : ''}
                {f.description}
              </div>
            ))}
          </div>
        </Tile>
      )}

      <Tile label="SESSION">
        {session ? (
          <div className="text-[clamp(16px,2.6vh,26px)]">
            {session.dry_run ? 'DRY RUN · ' : ''}
            {session.current_episode} / {session.num_episodes} episodes
          </div>
        ) : (
          <div className="text-dim text-[clamp(14px,2vh,18px)]">No active session</div>
        )}
      </Tile>

      <Tile label="DISK">
        <div className="text-[clamp(16px,2.6vh,26px)]">
          {snap ? `${gib(snap.storage.free)} free` : '—'}
        </div>
        {snap && (
          <div className="text-dim text-[12px] mt-1">
            {snap.storage.dataset_count} dataset(s)
          </div>
        )}
      </Tile>

      <div style={{ flex: 1 }} />

      {/* Stop lives at the bottom: it is the one control here, and the bottom of
          a vertical panel is what a standing person's hand reaches. */}
      <button
        type="button"
        onClick={onEstop}
        disabled={!session || estopBusy}
        className="w-full rounded-md text-center transition-opacity"
        style={{
          background: !session ? '#2a2a2a' : '#c81e1e',
          color: !session ? '#7a7a7a' : '#ffffff',
          border: `2px solid ${!session ? '#3a3a3a' : '#ff4d4d'}`,
          padding: '3vh 0',
          fontSize: 'clamp(22px,4.5vh,44px)',
          letterSpacing: '0.05em',
          cursor: !session ? 'not-allowed' : 'pointer',
          opacity: estopBusy ? 0.6 : 1,
        }}
      >
        {estopBusy ? 'STOPPING…' : 'EMERGENCY STOP'}
      </button>
      <div className="text-dim text-[clamp(11px,1.5vh,14px)] text-center leading-snug">
        {/* The claim tracks the hardware actually fitted: promising to halt a
            base on a rig that has none is the kind of detail that erodes trust
            in every other label on the screen. */}
        {session
          ? `${base ? 'Halts the base, homes' : 'Homes'} the arms, ends the session. ` +
            'Software stop — the physical button is the one that cuts power.'
          : 'No session running — nothing holds the hardware to stop.'}
      </div>
      {estopNote && (
        <div className="text-center text-[clamp(12px,1.7vh,16px)]" style={{ color: '#ffb020' }}>
          {estopNote}
        </div>
      )}
    </div>
  );
}
