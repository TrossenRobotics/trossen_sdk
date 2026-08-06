import { Server, Camera, Bot, Plus, Trash2, Edit, ChevronDown, ChevronUp, Radio, Smartphone, Save, Loader2, AlertTriangle, RotateCcw } from 'lucide-react';
import { useState, useEffect, useCallback, useRef } from 'react';
import { useBlocker } from 'react-router';
import { toast } from 'sonner';
import { AppModal } from '@/app/components/AppModal';
import { useConfirm } from '@/app/hooks/useConfirm';
import { useHwStatus } from '@/lib/HwStatusContext';
import { announce } from '@/lib/announce';
import { apiGet, apiPost, apiPut, describeError } from '@/lib/api';

// Level 3 - Producers (data recording channels)
interface Producer {
  id: string;
  type: 'camera' | 'arm' | 'base';
  hardware_id: string;
  stream_id: string;
  mode: 'poll' | 'push'; // Auto-determined by hardware type
  poll_rate_hz?: number; // Only for poll mode
  timeout_ms?: number; // Only for push mode (default 3000)
  use_device_time: boolean;
  encoding?: 'bgr8' | 'rgb8' | 'mono8'; // Camera producers only
}

// ZED capture modes, with the frame size each one negotiates to. The ZED
// component sets its resolution from this named string and ignores `width` /
// `height` in the config entirely, so the form offers the names and derives the
// pixel dimensions from them rather than letting an operator type numbers the
// camera will disregard. AUTO is resolved by the SDK at open time, so it has no
// dimensions to derive.
const ZED_RESOLUTIONS = {
  HD2K: { width: 2208, height: 1242 },
  HD1200: { width: 1920, height: 1200 },
  HD1080: { width: 1920, height: 1080 },
  HD720: { width: 1280, height: 720 },
  SVGA: { width: 960, height: 600 },
  VGA: { width: 672, height: 376 },
  AUTO: null,
} as const;
type ZedResolution = keyof typeof ZED_RESOLUTIONS;
const ZED_RESOLUTION_NAMES = Object.keys(ZED_RESOLUTIONS) as ZedResolution[];
// Matches the ZED component's own fallback, so a config with no resolution key
// keeps behaving the way it already did.
const DEFAULT_ZED_RESOLUTION: ZedResolution = 'HD720';

// ---------------------------------------------------------------------------
// Closed value sets the SDK validates against. Every one of these is rejected
// at configure() time if it does not match exactly, so they belong in a
// dropdown rather than a text field: the page previously defaulted a new arm to
// model "ViperX-300" / end effector "Gripper", neither of which the driver
// knows, so an arm added from the UI could not boot at all.
// ---------------------------------------------------------------------------

// Mirrors trossen_arm::MODEL_NAME in the installed driver. TrossenArmComponent
// resolves `model` against that map and throws listing the valid names, so this
// list must track the driver rather than the other way round.
// TeleopConfig::rate_hz — the mirror loop rate. 1 kHz is what every shipped
// preset uses; the arms' own control loop is what actually bounds this.
const DEFAULT_TELEOP_RATE_HZ = 1000.0;

const ARM_MODELS = [
  'wxai_v0',
  'vxai_v0_left',
  'vxai_v0_right',
  'core',
  'pro',
  'glide_left',
  'glide_right',
] as const;
type ArmModel = (typeof ARM_MODELS)[number];

// TrossenArmComponent hardcodes these three and throws on anything else. The
// choice also decides leader vs follower internally: `wxai_v0_leader` sets
// is_leader_, the other two clear it. `pro_base` matters beyond naming — using
// a wxai end effector on a Pro loads the wrong mass into gravity compensation,
// which the arm does not report as an error, it just holds position badly.
const END_EFFECTORS = ['wxai_v0_leader', 'wxai_v0_follower', 'pro_base'] as const;
type EndEffector = (typeof END_EFFECTORS)[number];

// TeleopPairConfig::space. "joint" is the default; a pair naming anything else
// needs that space implemented or the pair is skipped.
const TELEOP_SPACES = ['joint', 'cartesian'] as const;
type TeleopSpace = (typeof TELEOP_SPACES)[number];

// GlideSessionControlComponent::event_from_name. `stop_early` is valid and
// currently unused by any shipped preset; binding a button to "no event" is
// deliberately not accepted by the SDK, so there is no empty option.
const SESSION_CONTROL_EVENTS = [
  'start',
  'stop_early',
  'rerecord',
  'stop_session',
] as const;
type SessionControlEvent = (typeof SESSION_CONTROL_EVENTS)[number];

/** Human labels for the button events; the wire values stay as the SDK spells them. */
const SESSION_CONTROL_EVENT_LABELS: Readonly<Record<SessionControlEvent, string>> = {
  start: 'Start / next episode',
  stop_early: 'Stop episode early',
  rerecord: 'Re-record episode',
  stop_session: 'Stop session',
};

/** Short forms for the button cross, where a full label will not fit. */
const SESSION_CONTROL_EVENT_SHORT: Readonly<Record<SessionControlEvent, string>> = {
  start: 'Start',
  stop_early: 'Stop ep.',
  rerecord: 'Re-record',
  stop_session: 'Stop',
};

// The handle's four buttons sit in a cross, and the config stores each one as
// a BIT INDEX into the driver's InputReport.buttons byte, not as a position.
// Which bit is which button is a property of the handle hardware and appears
// nowhere in the SDK, so this table is the only place the two are tied
// together. Confirmed against the physical handle.
//
// Only 0-3 exist: `buttons` is a uint8_t and the driver sizes
// button_led_effects at 4. Nothing validates the number, and the SDK's
// accessor returns false for anything it does not recognise, so a bit typed by
// hand outside this range yields a button that is silently dead. Picking from
// the cross makes that unrepresentable.
const GLIDE_BUTTON_LAYOUT = [
  { bit: 0, label: 'Top', cell: 'col-start-2 row-start-1' },
  { bit: 1, label: 'Right', cell: 'col-start-3 row-start-2' },
  { bit: 2, label: 'Bottom', cell: 'col-start-2 row-start-3' },
  { bit: 3, label: 'Left', cell: 'col-start-1 row-start-2' },
] as const;

const GLIDE_BUTTON_BITS: ReadonlySet<number> = new Set(GLIDE_BUTTON_LAYOUT.map((b) => b.bit));

function asArmModel(value: unknown): ArmModel | undefined {
  return typeof value === 'string' && (ARM_MODELS as readonly string[]).includes(value)
    ? (value as ArmModel)
    : undefined;
}

function asEndEffector(value: unknown): EndEffector | undefined {
  return typeof value === 'string' && (END_EFFECTORS as readonly string[]).includes(value)
    ? (value as EndEffector)
    : undefined;
}

function asTeleopSpace(value: unknown): TeleopSpace | undefined {
  return typeof value === 'string' && (TELEOP_SPACES as readonly string[]).includes(value)
    ? (value as TeleopSpace)
    : undefined;
}

function asSessionControlEvent(value: unknown): SessionControlEvent | undefined {
  return typeof value === 'string' &&
    (SESSION_CONTROL_EVENTS as readonly string[]).includes(value)
    ? (value as SessionControlEvent)
    : undefined;
}

// Depth modes the ZED component recognises, cheapest first. The
// PERFORMANCE / QUALITY / ULTRA family it still accepts is deprecated in ZED
// SDK 5.x and deliberately not offered. Case matters: the component compares
// these strings exactly and an unrecognised one falls back to NEURAL with only
// a stderr warning, which is invisible from the webapp.
const ZED_DEPTH_MODES = ['NEURAL_LIGHT', 'NEURAL', 'NEURAL_PLUS'] as const;
type ZedDepthMode = (typeof ZED_DEPTH_MODES)[number];
const DEFAULT_ZED_DEPTH_MODE: ZedDepthMode = 'NEURAL_LIGHT';

// Depth-mode spellings written by older builds of this page (lowercase) or
// deprecated by StereoLabs (uppercase), mapped onto the NEURAL family per the
// component's own migration warnings. Read-only healing: a config carrying one
// of these is upgraded on load rather than silently falling back at open time.
const LEGACY_ZED_DEPTH_MODES: Readonly<Record<string, ZedDepthMode>> = {
  performance: 'NEURAL_LIGHT',
  PERFORMANCE: 'NEURAL_LIGHT',
  quality: 'NEURAL',
  QUALITY: 'NEURAL',
  ultra: 'NEURAL_PLUS',
  ULTRA: 'NEURAL_PLUS',
};

function asZedResolution(value: unknown): ZedResolution | undefined {
  return typeof value === 'string' && value in ZED_RESOLUTIONS
    ? (value as ZedResolution)
    : undefined;
}

function asZedDepthMode(value: unknown): ZedDepthMode | undefined {
  if (typeof value !== 'string') return undefined;
  if ((ZED_DEPTH_MODES as readonly string[]).includes(value)) return value as ZedDepthMode;
  return LEGACY_ZED_DEPTH_MODES[value];
}

// Level 2 - Hardware (physical devices)
interface CameraHardware {
  id: string;
  name: string;
  type: 'realsense_camera' | 'opencv_camera' | 'zed_camera';
  width: number;
  height: number;
  fps: number;
  // RealSense + ZED
  serial_number?: string;
  use_depth?: boolean;
  // OpenCV specific
  device_index?: string;
  backend?: string;
  warmup_frames?: number;
  // ZED specific. `resolution` is the only thing that sets a ZED's frame size,
  // and `depth_mode` is consulted only when use_depth is on — depth is gated
  // entirely on that flag, so a mode without it does nothing.
  resolution?: ZedResolution;
  depth_mode?: ZedDepthMode;
  producers: Producer[];
}

interface ArmHardware {
  id: string;
  name: string;
  type: 'trossen_arm';
  ip_address: string;
  model: string;
  end_effector: string;
  role: 'leader' | 'follower';
  paired_with?: string; // ID of paired arm
  // Passive leader (lightweight, no actuators). undefined/true = a normal
  // actuated arm. When false, the SDK skips teleop motion commands and applies
  // the joint remap below.
  actuated?: boolean;
  joint_signs?: number[];
  joint_offsets?: number[];
  // Leader-only gripper force feedback. When enabled, the leader's (actuated)
  // gripper renders a reflected force from the follower's measured gripper
  // effort, so the operator feels the grasp. The follower gripper stays plain
  // position passthrough. undefined/false = no feedback.
  gripper_force_feedback?: boolean;
  gripper_feedback_leader_max?: number;
  gripper_feedback_follower_max?: number;
  gripper_feedback_offset?: number;
  // Optional per-joint operating limits pushed to the controller on connect.
  // Each array, when set, has one entry per joint (arm joints in rad / rad·s⁻¹
  // / N·m, gripper in m / m·s⁻¹ / N). The controller resets these on power
  // cycle, so the SDK re-applies them every reconnect. undefined = leave the
  // firmware default untouched.
  position_min?: number[];
  position_max?: number[];
  velocity_max?: number[];
  effort_max?: number[];
  // Optional per-joint limit tolerances. These PAD the limits for the
  // controller's feedback fault check: it errors if actual position/velocity/
  // effort exceeds the limit ± tolerance. undefined = leave the firmware
  // default untouched; 0 = fault on any overshoot.
  position_tolerance?: number[];
  velocity_tolerance?: number[];
  effort_tolerance?: number[];
  // Optional per-joint clamp on the commands teleop sends to this arm, applied
  // before smoothing. `null` for a joint leaves it unclamped, which is the
  // normal case — a rig usually bounds one axis (J0, to keep a follower out of
  // its neighbour's space) and leaves the rest alone.
  //
  // Not the same as position_min/position_max above. Those are the arm's
  // operating limits and the CONTROLLER enforces them. These bound what we ask
  // for, so an out-of-range leader pose is trimmed host-side and never becomes
  // a limit fault that stops the session.
  command_position_min?: (number | null)[];
  command_position_max?: (number | null)[];
  // Trajectory time the controller is given to reach each teleop command, in
  // seconds. 0 = apply it immediately (the driver treats a goal time under
  // 1 ms as no interpolation), which is what a real-time mirror wants.
  //
  // Non-zero is a lag, not a smoother: teleop issues a new command every tick,
  // so at 1000 Hz a 0.3s goal time is superseded before it is 0.4% complete and
  // the follower perpetually chases a point 0.3s ahead. Use the One-Euro filter
  // below to damp jitter instead — it does not trade away tracking.
  //
  // Leaders are never commanded, so this has no effect on one.
  write_moving_time_s?: number;
  // Optional One-Euro adaptive low-pass filter on outgoing position commands.
  // Off by default. Cuts teleop jitter without the constant lag of a fixed
  // low-pass: the cutoff rises with the command's rate of change, so slow
  // motion is smoothed hard and fast motion is left alone.
  smoothing_enabled?: boolean;
  smoothing_gripper?: boolean;
  smoothing_min_cutoff_hz?: number;
  smoothing_beta?: number;
  smoothing_d_cutoff_hz?: number;
  producers: Producer[];
}

interface BaseHardware {
  id: string;
  name: string;
  // Two different bases, declared in two different places in the SDK config.
  // `slate_base` is the SLATE, which lives in the legacy `hardware.mobile_base`
  // slot. `trossen_base` is the Rivet's holonomic swerve base with a vertical
  // lift, which lives in `hardware.components` like every other decomposed
  // component.
  type: 'slate_base' | 'trossen_base';
  reset_odometry: boolean;
  enable_torque: boolean;
  // trossen_base only. Per-axis velocity ceilings the component clamps every
  // command to, plus the battery percentage below which the SDK trips the
  // software e-stop. The lift is the vertical rail, in driver units/s rather
  // than m/s because the driver exposes it that way.
  max_linear_mps?: number;
  max_angular_rps?: number;
  max_lift_units_per_s?: number;
  estop_battery_percent?: number;
  // How long configure() waits for the base to report ready before throwing.
  ready_timeout_s?: number;
  // The rail's ceiling is declared TWICE: as max_lift_units_per_s here, which
  // the base clamps every command to, and as the glide_base leader's
  // `axes.lift.max`, which scales the command before it ever reaches the base.
  // Raising one alone changes nothing — you stay capped by the other. These two
  // carry the leader's side so the form can show whether they agree and keep
  // them in step on save. `lift_leader_id` is the component the value came from.
  lift_leader_max?: number;
  lift_leader_id?: string;
  producers: Producer[];
}

type Hardware = CameraHardware | ArmHardware | BaseHardware;

// Level 1 - Hardware System (top-level grouping)
/** One leader -> follower link in `teleop.pairs`. */
interface TeleopPair {
  leader: string;
  follower: string;
  space: TeleopSpace;
}

/**
 * The `teleop` block, which this page now owns rather than passing through.
 *
 * It used to be copied verbatim from the original config, so deleting an arm
 * left its pairs behind (pointing at hardware that no longer existed) and
 * ADDING arms never created any. Both failed at record time, not at save time.
 */
interface TeleopModel {
  enabled: boolean;
  rate_hz: number;
  pairs: TeleopPair[];
}

/** A `glide_arm_input` component: which handle arms feed the input layer. */
interface GlideInputsModel {
  id: string;
  arms: string[];
}

/** One button binding inside `glide_session_control`. */
interface SessionButton {
  arm_id: string;
  bit: number;
  event: SessionControlEvent;
}

/** A `glide_session_control` component: handle buttons -> session events. */
interface SessionControlModel {
  id: string;
  poll_rate_hz?: number;
  debounce_ms?: number;
  buttons: SessionButton[];
}

interface HardwareSystem {
  id: string;
  name: string;
  description?: string;
  hardware: Hardware[];
  // Modelled config that does not belong to a single hardware card. Anything
  // NOT modelled here (glide_base, trossen_base, future component types) is
  // still carried through untouched on save.
  teleop?: TeleopModel;
  glideInputs?: GlideInputsModel;
  sessionControl?: SessionControlModel;
}

// Systems that ship with a factory-default config the user can revert to.
// Hoisted out of the component so the reset useCallback's dependency array
// stays stable across renders.
const RESETTABLE_SYSTEMS: readonly string[] = ['solo', 'solo_glide', 'stationary', 'mobile', 'workbench', 'rivet'];

// The lightweight (passive) Trossen leader has no actuators and its joints
// don't map 1:1 onto the follower: J3/J4 are inverted and the wrist (J5)
// carries a ∓π/4 offset whose sign mirrors between the left and right arms.
// The gripper (last element) passes through 1:1. The SDK applies this as an
// affine remap on the leader's read positions; we just carry the constants.
const LIGHTWEIGHT_LEADER_JOINT_SIGNS: readonly number[] = [1, 1, 1, -1, -1, 1, 1];
const LIGHTWEIGHT_LEADER_WRIST_OFFSET = Math.PI / 4;

type WristSide = 'left' | 'right';

function lightweightLeaderRemap(wristSide: WristSide): {
  joint_signs: number[];
  joint_offsets: number[];
} {
  // Right arm offsets +π/4 on the wrist, left −π/4, matching the mirror-mounted
  // hardware. The side is an explicit per-arm choice (set in the UI) rather than
  // inferred from the name, because which physical arm is left vs right depends
  // on the operator's setup, not the label.
  const wrist = wristSide === 'right'
    ? LIGHTWEIGHT_LEADER_WRIST_OFFSET
    : -LIGHTWEIGHT_LEADER_WRIST_OFFSET;
  return {
    joint_signs: [...LIGHTWEIGHT_LEADER_JOINT_SIGNS],
    joint_offsets: [0, 0, 0, 0, 0, wrist, 0],
  };
}

// Recover the wrist side from a stored joint_offsets array so the edit form
// shows the side that's actually persisted. A positive J5 offset (+π/4) is the
// right side; anything else (incl. absent) defaults to left.
function wristSideFromOffsets(offsets?: number[]): WristSide {
  return offsets && offsets.length > 5 && offsets[5] > 0 ? 'right' : 'left';
}

// Per-joint operating limits (wxai_v0: 6 arm joints + gripper). The controller
// clips commands to these and does NOT persist them across a power cycle, so the
// SDK re-applies them on every connect. The defaults below are deliberately
// PERMISSIVE starting points (wide position range, generous velocity/effort) —
// meant to be tightened per arm in the UI. Starting permissive means simply
// enabling limits can't by itself clip motion an existing setup relied on.
const ARM_JOINT_LABELS: readonly string[] = ['J1', 'J2', 'J3', 'J4', 'J5', 'J6', 'Gripper'];
const NUM_ARM_JOINTS = ARM_JOINT_LABELS.length;
interface JointLimitArrays {
  position_min: number[];
  position_max: number[];
  velocity_max: number[];
  effort_max: number[];
}
// Pick a stored per-joint limit array for the edit form, falling back to the
// permissive default when absent or the wrong length (e.g. a config from a
// different arm model). Always returns a fresh copy so form edits don't mutate
// the persisted system object in place.
function armLimitOrDefault(stored: number[] | undefined, key: keyof JointLimitArrays): number[] {
  return Array.isArray(stored) && stored.length === NUM_ARM_JOINTS
    ? [...stored]
    : [...DEFAULT_JOINT_LIMITS[key]];
}

/**
 * Pick a stored command-clamp column for the edit form.
 *
 * Unlike the limit arrays above there is no sensible default to fall back on:
 * inventing one would silently bound a joint nobody asked to bound. So a
 * missing or wrong-length column becomes all-blank, i.e. unclamped.
 */
function clampColumnOrBlank(stored: (number | null)[] | undefined): (number | null)[] {
  return Array.isArray(stored) && stored.length === NUM_ARM_JOINTS
    ? stored.map((v) => (typeof v === 'number' ? v : null))
    : Array<number | null>(NUM_ARM_JOINTS).fill(null);
}

const DEFAULT_JOINT_LIMITS: JointLimitArrays = {
  //             J1     J2     J3     J4     J5     J6    Gripper
  position_min: [-3.14, -3.14, -3.14, -3.14, -3.14, -3.14, 0.0],
  position_max: [3.14, 3.14, 3.14, 3.14, 3.14, 3.14, 0.05],
  velocity_max: [5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 0.2],
  effort_max: [40, 40, 40, 40, 40, 40, 80],
};

// Units shown per limit column. Arm joints are angular (rad), the gripper is
// linear (m); the table labels the gripper row separately.
type LimitFormKey = 'positionMin' | 'positionMax' | 'velocityMax' | 'effortMax';
const LIMIT_COLUMNS: readonly { formKey: LimitFormKey; label: string; armUnit: string; gripperUnit: string }[] = [
  { formKey: 'positionMin', label: 'Pos min', armUnit: 'rad', gripperUnit: 'm' },
  { formKey: 'positionMax', label: 'Pos max', armUnit: 'rad', gripperUnit: 'm' },
  { formKey: 'velocityMax', label: 'Vel max', armUnit: 'rad/s', gripperUnit: 'm/s' },
  { formKey: 'effortMax', label: 'Eff max', armUnit: 'N·m', gripperUnit: 'N' },
];

interface JointToleranceArrays {
  position_tolerance: number[];
  velocity_tolerance: number[];
  effort_tolerance: number[];
}
// Tolerances pad the limits for the controller's feedback fault check. Per the
// Trossen docs, a new/untested setup should start at 0.0 (fault on any
// overshoot to catch problems); raise them once tuned to avoid false positives.
// So — unlike the permissive limit defaults — tolerances default to 0.
const DEFAULT_JOINT_TOLERANCES: JointToleranceArrays = {
  //                  J1   J2   J3   J4   J5   J6  Gripper
  position_tolerance: [0, 0, 0, 0, 0, 0, 0],
  velocity_tolerance: [0, 0, 0, 0, 0, 0, 0],
  effort_tolerance: [0, 0, 0, 0, 0, 0, 0],
};
// One-Euro command-smoothing defaults, kept in step with the member
// initialisers in TrossenArmComponent (min cutoff 1.0 Hz, beta 0.9, derivative
// cutoff 1.0 Hz). Enabling smoothing in the UI without touching the tuning must
// produce the same behaviour as enabling it in a hand-written config.
// Explicitly typed rather than `as const`: these seed editable form fields, and
// literal types would make the form state un-writable.
const SMOOTHING_DEFAULTS: { min_cutoff_hz: number; beta: number; d_cutoff_hz: number } = {
  min_cutoff_hz: 1.0,
  beta: 0.9,
  d_cutoff_hz: 1.0,
};

// Same fallback semantics as armLimitOrDefault, for the tolerance arrays.
function armToleranceOrDefault(stored: number[] | undefined, key: keyof JointToleranceArrays): number[] {
  return Array.isArray(stored) && stored.length === NUM_ARM_JOINTS
    ? [...stored]
    : [...DEFAULT_JOINT_TOLERANCES[key]];
}
type ToleranceFormKey = 'positionTolerance' | 'velocityTolerance' | 'effortTolerance';
// Which tolerance columns the arm actually declares. Tracked per column because
// the three are independent in the config — a rig that sets velocity and effort
// tolerances but leaves position on the firmware default is normal, and saving
// it must not invent `position_tolerance: [0, 0, ...]`. A zero tolerance is the
// TIGHTEST possible feedback fault check, not a neutral placeholder, so
// inventing one silently makes an arm more likely to fault mid-episode.
type ToleranceSetKey = 'positionToleranceSet' | 'velocityToleranceSet' | 'effortToleranceSet';
const TOLERANCE_COLUMNS: readonly { formKey: ToleranceFormKey; setKey: ToleranceSetKey; label: string; armUnit: string; gripperUnit: string }[] = [
  { formKey: 'positionTolerance', setKey: 'positionToleranceSet', label: 'Pos tol', armUnit: 'rad', gripperUnit: 'm' },
  { formKey: 'velocityTolerance', setKey: 'velocityToleranceSet', label: 'Vel tol', armUnit: 'rad/s', gripperUnit: 'm/s' },
  { formKey: 'effortTolerance', setKey: 'effortToleranceSet', label: 'Eff tol', armUnit: 'N·m', gripperUnit: 'N' },
];

// ---------------------------------------------------------------------------
// Raw wire shapes for the SDK config blob.
// These mirror the JSON layout returned by /api/systems and accepted by
// PUT /api/systems/{id}. Fields are intentionally optional / loose so older
// or newer SDK versions can round-trip extra keys we don't recognise.
// ---------------------------------------------------------------------------

interface RawProducer {
  hardware_id?: string;
  stream_id?: string;
  type?: string;
  poll_rate_hz?: number;
  timeout_ms?: number;
  use_device_time?: boolean;
  encoding?: string;
  [key: string]: unknown;
}

interface RawArmConfig {
  ip_address?: string;
  model?: string;
  end_effector?: string;
  actuated?: boolean;
  joint_signs?: number[];
  joint_offsets?: number[];
  gripper_force_feedback?: boolean;
  gripper_feedback_leader_max?: number;
  gripper_feedback_follower_max?: number;
  gripper_feedback_offset?: number;
  position_min?: number[];
  position_max?: number[];
  velocity_max?: number[];
  effort_max?: number[];
  position_tolerance?: number[];
  velocity_tolerance?: number[];
  effort_tolerance?: number[];
  command_position_min?: (number | null)[];
  command_position_max?: (number | null)[];
  write_moving_time_s?: number;
  smoothing_enabled?: boolean;
  smoothing_gripper?: boolean;
  smoothing_min_cutoff_hz?: number;
  smoothing_beta?: number;
  smoothing_d_cutoff_hz?: number;
  [key: string]: unknown;
}

interface RawCameraConfig {
  id?: string;
  // The hardware registry key. This is what the SDK actually constructs the
  // camera from, and CameraConfig defaults it to "realsense_camera" when
  // absent — so it MUST be written back out on save. Omitting it silently
  // turns every ZED into a RealSense.
  type?: string;
  serial_number?: string | number;
  width?: number;
  height?: number;
  fps?: number;
  use_depth?: boolean;
  device_index?: number | string;
  backend?: string;
  warmup_frames?: number;
  depth_mode?: string;
  [key: string]: unknown;
}

interface RawMobileBaseConfig {
  reset_odometry?: boolean;
  enable_torque?: boolean;
  [key: string]: unknown;
}

/**
 * An entry in `hardware.components` — the decomposed-config slot for anything
 * that is neither an arm nor a camera: the Rivet's base, the Glide input
 * handles, the base leader, session control.
 *
 * Only `id` and `type` are interpreted here. Every other key is opaque to this
 * page and is round-tripped byte-for-byte on save, because the page has no way
 * to know what a component it does not model needs in order to work.
 */
interface RawComponentConfig {
  id?: string;
  type?: string;
  [key: string]: unknown;
}

/**
 * Reads `axes.lift.max` off a glide_base leader component, or undefined when
 * the component has no lift axis.
 *
 * The shape is opaque to this page (glide_base is round-tripped verbatim), so
 * this walks it defensively rather than casting: a leader with no lift axis is
 * a normal configuration, not an error.
 */
function readLiftAxisMax(comp: RawComponentConfig): number | undefined {
  const axes = comp.axes;
  if (typeof axes !== 'object' || axes === null) return undefined;
  const lift = (axes as Record<string, unknown>).lift;
  if (typeof lift !== 'object' || lift === null) return undefined;
  const max = (lift as Record<string, unknown>).max;
  return typeof max === 'number' ? max : undefined;
}

/**
 * Returns a copy of a glide_base component with `axes.lift.max` set, preserving
 * every other key at every level.
 *
 * Only called for a component `readLiftAxisMax` already accepted, so the nested
 * objects are known to exist.
 */
function withLiftAxisMax(comp: RawComponentConfig, max: number): RawComponentConfig {
  const axes = comp.axes as Record<string, unknown>;
  const lift = axes.lift as Record<string, unknown>;
  return { ...comp, axes: { ...axes, lift: { ...lift, max } } };
}

interface RawSdkHardware {
  arms?: Record<string, RawArmConfig>;
  cameras?: RawCameraConfig[];
  mobile_base?: RawMobileBaseConfig;
  components?: RawComponentConfig[];
}

interface RawSdkConfig {
  robot_name?: string;
  hardware?: RawSdkHardware;
  producers?: RawProducer[];
  teleop?: unknown;
  backend?: unknown;
  session?: unknown;
  [key: string]: unknown;
}

interface RawSystemResponse {
  id: string;
  name?: string;
  config?: RawSdkConfig;
  hw_status?: string;
  hw_message?: string;
}

// ---------------------------------------------------------------------------
// SDK config <-> UI model conversion
// ---------------------------------------------------------------------------

/**
 * Converts an API system response into the flat UI representation.
 *
 * The SDK config nests arms as a keyed object, cameras as an array, and an
 * optional mobile_base object.  Producers live at the top-level `producers`
 * array keyed by `hardware_id`.  This function denormalises that structure
 * into a flat `Hardware[]` list where each item carries its own producers.
 */
export function sdkConfigToSystem(id: string, apiData: RawSystemResponse): HardwareSystem {
  const config: RawSdkConfig = apiData.config ?? {};
  const hw: RawSdkHardware = config.hardware ?? {};
  const sdkProducers: RawProducer[] = config.producers ?? [];
  const hardwareItems: Hardware[] = [];

  // --- Arms (keyed object) ---------------------------------------------------
  const arms: Record<string, RawArmConfig> = hw.arms ?? {};
  for (const [key, armCfg] of Object.entries(arms)) {
    const armId = key; // The key IS the arm name/id in SDK config
    // Role is derived from the arm's config key, which is the only place the
    // SDK records it — there is no `role` field. `glide` counts because the
    // Rivet/Workbench handles are leaders whose keys never say so
    // (`glide_left`, not `leader_left`).
    const keyLower = key.toLowerCase();
    const role: 'leader' | 'follower' =
      keyLower.includes('leader') || keyLower.includes('glide')
        ? 'leader'
        : 'follower';

    // Collect producers whose hardware_id matches this arm key
    const armProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === key)
      .map((p) => ({
        id: `prod-${key}-${p.stream_id}`,
        type: 'arm' as const,
        hardware_id: armId,
        stream_id: p.stream_id ?? key,
        mode: 'poll' as const, // trossen_arm is always poll
        poll_rate_hz: p.poll_rate_hz,
        use_device_time: p.use_device_time ?? false,
      }));

    hardwareItems.push({
      id: armId,
      name: key,
      type: 'trossen_arm',
      ip_address: armCfg.ip_address ?? '',
      model: armCfg.model ?? '',
      end_effector: armCfg.end_effector ?? '',
      role,
      actuated: typeof armCfg.actuated === 'boolean' ? armCfg.actuated : undefined,
      joint_signs: Array.isArray(armCfg.joint_signs) ? armCfg.joint_signs : undefined,
      joint_offsets: Array.isArray(armCfg.joint_offsets) ? armCfg.joint_offsets : undefined,
      gripper_force_feedback: typeof armCfg.gripper_force_feedback === 'boolean' ? armCfg.gripper_force_feedback : undefined,
      gripper_feedback_leader_max: typeof armCfg.gripper_feedback_leader_max === 'number' ? armCfg.gripper_feedback_leader_max : undefined,
      gripper_feedback_follower_max: typeof armCfg.gripper_feedback_follower_max === 'number' ? armCfg.gripper_feedback_follower_max : undefined,
      gripper_feedback_offset: typeof armCfg.gripper_feedback_offset === 'number' ? armCfg.gripper_feedback_offset : undefined,
      position_min: Array.isArray(armCfg.position_min) ? armCfg.position_min : undefined,
      position_max: Array.isArray(armCfg.position_max) ? armCfg.position_max : undefined,
      velocity_max: Array.isArray(armCfg.velocity_max) ? armCfg.velocity_max : undefined,
      effort_max: Array.isArray(armCfg.effort_max) ? armCfg.effort_max : undefined,
      position_tolerance: Array.isArray(armCfg.position_tolerance) ? armCfg.position_tolerance : undefined,
      velocity_tolerance: Array.isArray(armCfg.velocity_tolerance) ? armCfg.velocity_tolerance : undefined,
      effort_tolerance: Array.isArray(armCfg.effort_tolerance) ? armCfg.effort_tolerance : undefined,
      command_position_min: Array.isArray(armCfg.command_position_min) ? armCfg.command_position_min : undefined,
      command_position_max: Array.isArray(armCfg.command_position_max) ? armCfg.command_position_max : undefined,
      write_moving_time_s: typeof armCfg.write_moving_time_s === 'number' ? armCfg.write_moving_time_s : undefined,
      smoothing_enabled: typeof armCfg.smoothing_enabled === 'boolean' ? armCfg.smoothing_enabled : undefined,
      smoothing_gripper: typeof armCfg.smoothing_gripper === 'boolean' ? armCfg.smoothing_gripper : undefined,
      smoothing_min_cutoff_hz: typeof armCfg.smoothing_min_cutoff_hz === 'number' ? armCfg.smoothing_min_cutoff_hz : undefined,
      smoothing_beta: typeof armCfg.smoothing_beta === 'number' ? armCfg.smoothing_beta : undefined,
      smoothing_d_cutoff_hz: typeof armCfg.smoothing_d_cutoff_hz === 'number' ? armCfg.smoothing_d_cutoff_hz : undefined,
      producers: armProducers,
    } as ArmHardware);
  }

  // --- Cameras (array) -------------------------------------------------------
  const cameras: RawCameraConfig[] = hw.cameras ?? [];
  for (const camCfg of cameras) {
    const camId = camCfg.id ?? `cam-${camCfg.serial_number ?? Date.now()}`;

    // Prefer the hardware entry's own `type`, since that is the field the SDK
    // builds the camera from. Fall back to the matching producer's type for
    // older configs that only carry it there, and to realsense_camera when
    // neither says — matching CameraConfig's own default.
    const matchingProducer = sdkProducers.find((p) => p.hardware_id === camCfg.id);
    const declaredType = camCfg.type ?? matchingProducer?.type;
    const cameraType: CameraHardware['type'] =
      declaredType === 'opencv_camera'
        ? 'opencv_camera'
        : declaredType === 'zed_camera'
          ? 'zed_camera'
          : 'realsense_camera';

    // Determine producer mode: realsense & zed are push, everything else poll
    const producerMode: 'poll' | 'push' =
      cameraType === 'realsense_camera' || cameraType === 'zed_camera'
        ? 'push'
        : 'poll';

    const camProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === camCfg.id)
      .map((p) => ({
        id: `prod-${camCfg.id}-${p.stream_id}`,
        type: 'camera' as const,
        hardware_id: camId,
        stream_id: p.stream_id ?? String(camCfg.id ?? camId),
        mode: producerMode,
        poll_rate_hz: p.poll_rate_hz,
        timeout_ms: p.timeout_ms,
        use_device_time: p.use_device_time ?? false,
        ...(p.encoding != null && { encoding: p.encoding as Producer['encoding'] }),
      }));

    // A ZED negotiates its frame size from the named resolution and ignores any
    // width/height in the config, so derive the dimensions from the resolution
    // rather than surfacing numbers the camera never honoured. AUTO resolves at
    // open time and yields none, so fall back to whatever the config carries.
    const zedResolution =
      cameraType === 'zed_camera'
        ? (asZedResolution(camCfg.resolution) ?? DEFAULT_ZED_RESOLUTION)
        : undefined;
    const zedDims = zedResolution ? ZED_RESOLUTIONS[zedResolution] : null;

    hardwareItems.push({
      id: camId,
      name: camCfg.id ?? camId,
      type: cameraType,
      width: zedDims?.width ?? camCfg.width ?? 640,
      height: zedDims?.height ?? camCfg.height ?? 480,
      fps: camCfg.fps ?? 30,
      ...(cameraType === 'realsense_camera' && {
        serial_number: camCfg.serial_number,
        use_depth: camCfg.use_depth,
      }),
      ...(cameraType === 'opencv_camera' && {
        device_index: camCfg.device_index,
        backend: camCfg.backend,
        warmup_frames: camCfg.warmup_frames,
      }),
      ...(cameraType === 'zed_camera' && {
        // ZED serials are numeric and the SDK accepts them as JSON numbers, so
        // normalise to a string for the form to edit.
        serial_number: camCfg.serial_number != null ? String(camCfg.serial_number) : undefined,
        resolution: zedResolution,
        use_depth: camCfg.use_depth,
        depth_mode: asZedDepthMode(camCfg.depth_mode),
      }),
      producers: camProducers,
    } as CameraHardware);
  }

  // --- Mobile base (single object, optional) ---------------------------------
  if (hw.mobile_base) {
    const baseId = 'mobile_base';
    const baseProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === baseId || p.type === 'slate_base')
      .map((p) => ({
        id: `prod-${baseId}-${p.stream_id}`,
        type: 'base' as const,
        hardware_id: baseId,
        stream_id: p.stream_id ?? baseId,
        mode: 'poll' as const,
        poll_rate_hz: p.poll_rate_hz,
        use_device_time: p.use_device_time ?? false,
      }));

    hardwareItems.push({
      id: baseId,
      name: 'mobile_base',
      type: 'slate_base',
      reset_odometry: hw.mobile_base.reset_odometry ?? false,
      enable_torque: hw.mobile_base.enable_torque ?? false,
      producers: baseProducers,
    } as BaseHardware);
  }

  // --- Components: the Rivet's base ------------------------------------------
  // Everything else under `hardware.components` (glide_arm_input, glide_base,
  // glide_session_control) has no card on this page and is passed through
  // untouched by systemToSdkConfig. The base is surfaced because its velocity
  // ceilings and e-stop battery threshold are things an operator needs to see.
  const components: RawComponentConfig[] = hw.components ?? [];

  // The rail's other half. A glide_base leader declares `axes.lift.max`, which
  // scales the lift command before the base ever clamps it to its own ceiling —
  // so the base panel needs to see it to report whether the two agree.
  const liftLeader = components.find(
    (c) => c.type === 'glide_base' && readLiftAxisMax(c) !== undefined,
  );

  for (const comp of components) {
    if (comp.type !== 'trossen_base') continue;
    const baseId = comp.id ?? 'trossen_base';
    const baseProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === baseId)
      .map((p) => ({
        id: `prod-${baseId}-${p.stream_id}`,
        type: 'base' as const,
        hardware_id: baseId,
        stream_id: p.stream_id ?? baseId,
        mode: 'poll' as const,
        poll_rate_hz: p.poll_rate_hz,
        use_device_time: p.use_device_time ?? false,
      }));

    hardwareItems.push({
      id: baseId,
      name: baseId,
      type: 'trossen_base',
      // The Rivet base has no odometry-reset or torque-enable knob; those are
      // SLATE concepts. Fixed false so the shared BaseHardware shape holds.
      reset_odometry: false,
      enable_torque: false,
      max_linear_mps: typeof comp.max_linear_mps === 'number' ? comp.max_linear_mps : undefined,
      max_angular_rps: typeof comp.max_angular_rps === 'number' ? comp.max_angular_rps : undefined,
      max_lift_units_per_s: typeof comp.max_lift_units_per_s === 'number' ? comp.max_lift_units_per_s : undefined,
      estop_battery_percent: typeof comp.estop_battery_percent === 'number' ? comp.estop_battery_percent : undefined,
      ready_timeout_s: typeof comp.ready_timeout_s === 'number' ? comp.ready_timeout_s : undefined,
      lift_leader_max: liftLeader ? readLiftAxisMax(liftLeader) : undefined,
      lift_leader_id: typeof liftLeader?.id === 'string' ? liftLeader.id : undefined,
      producers: baseProducers,
    } as BaseHardware);
  }

  // --- Build description from hardware counts --------------------------------
  const armCount = Object.keys(arms).length;
  const camCount = cameras.length;
  const baseCount =
    (hw.mobile_base ? 1 : 0) + components.filter((c) => c.type === 'trossen_base').length;
  const parts: string[] = [];
  if (armCount > 0) parts.push(`${armCount} arm${armCount !== 1 ? 's' : ''}`);
  if (camCount > 0) parts.push(`${camCount} camera${camCount !== 1 ? 's' : ''}`);
  if (baseCount > 0) parts.push(`${baseCount} base`);
  const description = parts.length > 0 ? parts.join(', ') : 'No hardware';

  // --- teleop ---------------------------------------------------------------
  const rawTeleop = (config.teleop ?? {}) as Record<string, unknown>;
  const rawPairs = Array.isArray(rawTeleop.pairs) ? rawTeleop.pairs : [];
  const teleop: TeleopModel = {
    // TeleopConfig::enabled defaults to TRUE in the SDK (`bool enabled{true}`),
    // so an omitted key means on, not off.
    enabled: typeof rawTeleop.enabled === 'boolean' ? rawTeleop.enabled : true,
    rate_hz: typeof rawTeleop.rate_hz === 'number' ? rawTeleop.rate_hz : DEFAULT_TELEOP_RATE_HZ,
    pairs: rawPairs
      .filter((p): p is Record<string, unknown> => !!p && typeof p === 'object')
      .map((p) => ({
        leader: typeof p.leader === 'string' ? p.leader : '',
        follower: typeof p.follower === 'string' ? p.follower : '',
        // The SDK defaults an absent space to "joint"; mirror that rather than
        // emitting an empty string it would then reject.
        space: asTeleopSpace(p.space) ?? 'joint',
      }))
      .filter((p) => p.leader !== '' || p.follower !== ''),
  };

  // --- the two Glide components this page models ----------------------------
  const glideInputsCfg = components.find((c) => c.type === 'glide_arm_input');
  const glideInputs: GlideInputsModel | undefined = glideInputsCfg
    ? {
        id: String(glideInputsCfg.id ?? 'glide_inputs'),
        arms: Array.isArray(glideInputsCfg.arms)
          ? (glideInputsCfg.arms as unknown[]).filter((a): a is string => typeof a === 'string')
          : [],
      }
    : undefined;

  const sessionCfg = components.find((c) => c.type === 'glide_session_control');
  const sessionControl: SessionControlModel | undefined = sessionCfg
    ? {
        id: String(sessionCfg.id ?? 'session_control'),
        poll_rate_hz: typeof sessionCfg.poll_rate_hz === 'number' ? sessionCfg.poll_rate_hz : undefined,
        debounce_ms: typeof sessionCfg.debounce_ms === 'number' ? sessionCfg.debounce_ms : undefined,
        buttons: (Array.isArray(sessionCfg.buttons) ? sessionCfg.buttons : [])
          .filter((b): b is Record<string, unknown> => !!b && typeof b === 'object')
          .map((b) => ({
            arm_id: typeof b.arm_id === 'string' ? b.arm_id : '',
            bit: typeof b.bit === 'number' ? b.bit : 0,
            // An unknown event would be rejected by the SDK; keep the binding
            // visible by falling back rather than dropping the row silently.
            event: asSessionControlEvent(b.event) ?? 'start',
          })),
      }
    : undefined;

  return {
    id,
    name: apiData.name ?? config.robot_name ?? id,
    description,
    hardware: hardwareItems,
    teleop,
    glideInputs,
    sessionControl,
  };
}

/**
 * Converts the flat UI model back into the SDK config format for persistence.
 *
 * Sections not managed by the Configuration page (teleop, backend, session)
 * are preserved from the original config so they are not lost on save.
 */
/**
 * Build the `teleop` block, dropping pairs whose arms no longer exist.
 *
 * Pruning here rather than warning-and-emitting is deliberate: the SDK's teleop
 * factory only logs `[warn] Leader '...' not registered, skipping pair` and
 * carries on, so a dangling pair is invisible unless someone reads the bootstrap
 * output. A config that cannot work should not be written in the first place.
 *
 * `enabled` follows the pairs — a system with none (a camera-only rig) emits
 * `enabled: false` rather than an enabled block with nothing in it.
 */
/**
 * Add or remove one handle from the `glide_arm_input` component.
 *
 * Returns the patch rather than applying it, so the rule that governs it is
 * testable: the component throws in configure() when it names no arms
 * ("requires a non-empty 'arms' array"), so an empty list has to mean "do not
 * declare the component at all" rather than "declare an empty one". Turning the
 * last handle off therefore deletes the component.
 */
export function setGlideHandleInput(
  system: HardwareSystem,
  armName: string,
  on: boolean,
): Pick<HardwareSystem, 'glideInputs'> {
  const existing = system.glideInputs;
  const rest = (existing?.arms ?? []).filter((n) => n !== armName);
  const arms = on ? [...rest, armName] : rest;
  return {
    glideInputs: arms.length ? { id: existing?.id ?? 'glide_inputs', arms } : undefined,
  };
}

/**
 * Bind or clear one button on one handle.
 *
 * `event: null` clears. Same lifecycle rule as the handle input above — the SDK
 * rejects a `glide_session_control` that claims no buttons, so clearing the last
 * binding anywhere in the system removes the component instead of leaving an
 * empty one that fails at bring-up. Poll rate and debounce survive as long as
 * the component does, and are re-seeded from the SDK's own defaults when it is
 * created by the first binding.
 */
export function setGlideButtonBinding(
  system: HardwareSystem,
  armName: string,
  bit: number,
  event: SessionControlEvent | null,
): Pick<HardwareSystem, 'sessionControl'> {
  const existing = system.sessionControl;
  const others = (existing?.buttons ?? []).filter(
    (b) => !(b.arm_id === armName && b.bit === bit),
  );
  const buttons = event === null ? others : [...others, { arm_id: armName, bit, event }];
  return {
    sessionControl: buttons.length
      ? {
          id: existing?.id ?? 'session_control',
          poll_rate_hz: existing?.poll_rate_hz ?? 50,
          debounce_ms: existing?.debounce_ms ?? 40,
          buttons,
        }
      : undefined,
  };
}

function buildTeleop(
  system: HardwareSystem,
  armsObj: Record<string, RawArmConfig>,
  originalConfig: RawSdkConfig | undefined,
): Record<string, unknown> {
  const model = system.teleop;
  if (!model) return (originalConfig?.teleop as Record<string, unknown>) ?? {};
  const live = new Set(Object.keys(armsObj));
  const pairs = model.pairs
    .filter((p) => live.has(p.leader) && live.has(p.follower))
    .map((p) => ({ leader: p.leader, follower: p.follower, space: p.space }));
  return {
    // Spread first: TeleopConfig is a closed struct today, but silently
    // dropping keys this page does not model is exactly how the ZED settings
    // and the components block were lost before.
    ...((originalConfig?.teleop as Record<string, unknown>) ?? {}),
    enabled: model.enabled && pairs.length > 0,
    rate_hz: model.rate_hz,
    pairs,
  };
}

export function systemToSdkConfig(system: HardwareSystem, originalConfig: RawSdkConfig | undefined): RawSdkConfig {
  const armsObj: Record<string, RawArmConfig> = {};
  const camerasArr: RawCameraConfig[] = [];
  let mobileBase: RawMobileBaseConfig | undefined;
  let trossenBase: BaseHardware | undefined;
  const allProducers: RawProducer[] = [];

  for (const hw of system.hardware) {
    if (hw.type === 'trossen_arm') {
      const arm = hw as ArmHardware;
      const armEntry: RawArmConfig = {
        ip_address: arm.ip_address,
        model: arm.model,
        end_effector: arm.end_effector,
      };
      // Only emit passive-leader fields when set, so ordinary arms stay clean
      // (the SDK defaults actuated=true and identity remap).
      if (arm.actuated === false) armEntry.actuated = false;
      if (arm.joint_signs && arm.joint_signs.length) armEntry.joint_signs = arm.joint_signs;
      if (arm.joint_offsets && arm.joint_offsets.length) armEntry.joint_offsets = arm.joint_offsets;
      // Only emit gripper feedback tuning when enabled, so ordinary leaders
      // (no feedback) stay clean.
      if (arm.gripper_force_feedback) {
        armEntry.gripper_force_feedback = true;
        if (typeof arm.gripper_feedback_leader_max === 'number') armEntry.gripper_feedback_leader_max = arm.gripper_feedback_leader_max;
        if (typeof arm.gripper_feedback_follower_max === 'number') armEntry.gripper_feedback_follower_max = arm.gripper_feedback_follower_max;
        if (typeof arm.gripper_feedback_offset === 'number') armEntry.gripper_feedback_offset = arm.gripper_feedback_offset;
      }
      // Only emit per-joint limits that are actually set, so arms left at the
      // controller's firmware defaults stay clean in the config.
      if (arm.position_min && arm.position_min.length) armEntry.position_min = arm.position_min;
      if (arm.position_max && arm.position_max.length) armEntry.position_max = arm.position_max;
      if (arm.velocity_max && arm.velocity_max.length) armEntry.velocity_max = arm.velocity_max;
      if (arm.effort_max && arm.effort_max.length) armEntry.effort_max = arm.effort_max;
      // Per-joint tolerances, same emit-only-when-set rule.
      if (arm.position_tolerance && arm.position_tolerance.length) armEntry.position_tolerance = arm.position_tolerance;
      if (arm.velocity_tolerance && arm.velocity_tolerance.length) armEntry.velocity_tolerance = arm.velocity_tolerance;
      if (arm.effort_tolerance && arm.effort_tolerance.length) armEntry.effort_tolerance = arm.effort_tolerance;
      // The command clamp, emitted only when at least one joint is actually
      // bounded. An all-null array is the same as no array, and writing one
      // would suggest a clamp exists where none does.
      const clampSet = (a?: (number | null)[]) =>
        !!a && a.length > 0 && a.some((v) => typeof v === 'number');
      if (clampSet(arm.command_position_min)) armEntry.command_position_min = arm.command_position_min;
      if (clampSet(arm.command_position_max)) armEntry.command_position_max = arm.command_position_max;
      // Goal time, emitted only when it is actually asking for interpolation.
      // Absence and 0 mean the same thing to the SDK (`write_moving_time_s_`
      // defaults to 0.0f = apply immediately), so dropping a zero is lossless
      // and keeps a real-time arm — the common case — clean in the config.
      if (typeof arm.write_moving_time_s === 'number' && arm.write_moving_time_s > 0) {
        armEntry.write_moving_time_s = arm.write_moving_time_s;
      }
      // Command smoothing, same emit-only-when-on rule. The tuning rides along
      // only when smoothing is enabled, so an arm that never asked for it stays
      // clean in the config.
      if (arm.smoothing_enabled) {
        armEntry.smoothing_enabled = true;
        if (arm.smoothing_gripper) armEntry.smoothing_gripper = true;
        if (typeof arm.smoothing_min_cutoff_hz === 'number') armEntry.smoothing_min_cutoff_hz = arm.smoothing_min_cutoff_hz;
        if (typeof arm.smoothing_beta === 'number') armEntry.smoothing_beta = arm.smoothing_beta;
        if (typeof arm.smoothing_d_cutoff_hz === 'number') armEntry.smoothing_d_cutoff_hz = arm.smoothing_d_cutoff_hz;
      }
      armsObj[arm.name] = armEntry;

      for (const p of arm.producers) {
        allProducers.push({
          type: 'trossen_arm',
          hardware_id: arm.name,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          use_device_time: p.use_device_time,
        });
      }
    } else if (hw.type === 'slate_base') {
      const base = hw as BaseHardware;
      mobileBase = {
        reset_odometry: base.reset_odometry,
        enable_torque: base.enable_torque,
      };

      for (const p of base.producers) {
        allProducers.push({
          type: 'slate_base',
          hardware_id: base.id,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          use_device_time: p.use_device_time,
        });
      }
    } else if (hw.type === 'trossen_base') {
      // The base entry itself is merged back into hardware.components below,
      // where the keys this page does not model are preserved. Here we only
      // re-emit its producer.
      const base = hw as BaseHardware;
      trossenBase = base;

      for (const p of base.producers) {
        allProducers.push({
          type: 'trossen_base',
          hardware_id: base.id,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          use_device_time: p.use_device_time,
        });
      }
    } else {
      // Camera types: realsense_camera, opencv_camera, zed_camera
      const cam = hw as CameraHardware;
      const camEntry: RawCameraConfig = {
        id: cam.name,
        // Load-bearing: this is the field the SDK creates the camera from, and
        // CameraConfig defaults it to "realsense_camera" when it is missing.
        // Dropping it turns a ZED rig into a RealSense rig, which on a build
        // without librealsense2 fails as "Unsupported hardware type".
        type: cam.type,
        width: cam.width,
        height: cam.height,
        fps: cam.fps,
      };

      if (cam.type === 'realsense_camera') {
        if (cam.serial_number) camEntry.serial_number = cam.serial_number;
        if (cam.use_depth != null) camEntry.use_depth = cam.use_depth;
      } else if (cam.type === 'opencv_camera') {
        if (cam.device_index) camEntry.device_index = cam.device_index;
        if (cam.backend) camEntry.backend = cam.backend;
        if (cam.warmup_frames != null) camEntry.warmup_frames = cam.warmup_frames;
      } else if (cam.type === 'zed_camera') {
        if (cam.serial_number) camEntry.serial_number = cam.serial_number;
        // Both of these are load-bearing and both used to be dropped here.
        // `resolution` is the ONLY thing that sets a ZED's frame size — losing
        // it silently reverts an HD1200 rig to the component's HD720 default,
        // and width/height (emitted above for every camera type) are ignored by
        // the ZED, so nothing else carries the intent. Depth is gated entirely
        // on `use_depth`, so without it depth_mode is inert.
        camEntry.resolution = cam.resolution ?? DEFAULT_ZED_RESOLUTION;
        if (cam.use_depth != null) camEntry.use_depth = cam.use_depth;
        if (cam.depth_mode) camEntry.depth_mode = cam.depth_mode;
      }

      camerasArr.push(camEntry);

      for (const p of cam.producers) {
        allProducers.push({
          type: cam.type, // realsense_camera | opencv_camera | zed_camera
          hardware_id: cam.name,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          ...(p.timeout_ms != null && { timeout_ms: p.timeout_ms }),
          use_device_time: p.use_device_time,
          ...(p.encoding != null && { encoding: p.encoding }),
        });
      }
    }
  }

  const hardware: RawSdkHardware = { arms: armsObj, cameras: camerasArr };
  if (mobileBase) {
    hardware.mobile_base = mobileBase;
  }

  // Preserve `hardware.components` verbatim, merging in only the base fields
  // this page edits. Components are how the decomposed configs declare the
  // Rivet's base, the Glide input handles, the base leader and session control
  // — none of which this page models. Rebuilding the array from the UI would
  // therefore delete them, so start from the original and patch.
  const originalComponents = originalConfig?.hardware?.components;
  if (originalComponents?.length) {
    hardware.components = originalComponents.flatMap((comp) => {
      // The two Glide components ARE modelled now, so they are rebuilt from the
      // UI rather than preserved. Returning [] drops one the user removed —
      // which is the point: a config keeping `glide_arm_input` after its handle
      // arms were deleted fails every recording in configure().
      if (comp.type === 'glide_arm_input') {
        return system.glideInputs
          ? [{ ...comp, id: system.glideInputs.id, arms: [...system.glideInputs.arms] }]
          : [];
      }
      if (comp.type === 'glide_session_control') {
        if (!system.sessionControl) return [];
        const sc: RawComponentConfig = {
          ...comp,
          id: system.sessionControl.id,
          buttons: system.sessionControl.buttons.map((b) => ({ ...b })),
        };
        if (typeof system.sessionControl.poll_rate_hz === 'number') sc.poll_rate_hz = system.sessionControl.poll_rate_hz;
        if (typeof system.sessionControl.debounce_ms === 'number') sc.debounce_ms = system.sessionControl.debounce_ms;
        return [sc];
      }
      // The rail's ceiling is edited in one place on the base panel but has to
      // land in two: here on the leader, which scales the lift command, and on
      // the base, which clamps it. Keeping them in step is the whole point —
      // raising only one leaves the rail capped by the other.
      if (
        trossenBase &&
        typeof trossenBase.max_lift_units_per_s === 'number' &&
        comp.type === 'glide_base' &&
        comp.id === trossenBase.lift_leader_id &&
        readLiftAxisMax(comp) !== undefined
      ) {
        return [withLiftAxisMax(comp, trossenBase.max_lift_units_per_s)];
      }
      if (comp.type !== 'trossen_base' || !trossenBase || comp.id !== trossenBase.id) {
        return [comp];
      }
      const patched: RawComponentConfig = { ...comp };
      if (typeof trossenBase.max_linear_mps === 'number') patched.max_linear_mps = trossenBase.max_linear_mps;
      if (typeof trossenBase.max_angular_rps === 'number') patched.max_angular_rps = trossenBase.max_angular_rps;
      if (typeof trossenBase.max_lift_units_per_s === 'number') patched.max_lift_units_per_s = trossenBase.max_lift_units_per_s;
      if (typeof trossenBase.estop_battery_percent === 'number') patched.estop_battery_percent = trossenBase.estop_battery_percent;
      if (typeof trossenBase.ready_timeout_s === 'number') patched.ready_timeout_s = trossenBase.ready_timeout_s;
      return [patched];
    });
  }

  // A Glide component the original config never had — the user added it in the
  // UI — has nothing to patch, so append it.
  const existingTypes = new Set((hardware.components ?? []).map((c) => c.type));
  if (system.glideInputs && !existingTypes.has('glide_arm_input')) {
    hardware.components = [
      ...(hardware.components ?? []),
      { id: system.glideInputs.id, type: 'glide_arm_input', arms: [...system.glideInputs.arms] },
    ];
  }
  if (system.sessionControl && !existingTypes.has('glide_session_control')) {
    const sc: RawComponentConfig = {
      id: system.sessionControl.id,
      type: 'glide_session_control',
      buttons: system.sessionControl.buttons.map((b) => ({ ...b })),
    };
    if (typeof system.sessionControl.poll_rate_hz === 'number') sc.poll_rate_hz = system.sessionControl.poll_rate_hz;
    if (typeof system.sessionControl.debounce_ms === 'number') sc.debounce_ms = system.sessionControl.debounce_ms;
    hardware.components = [...(hardware.components ?? []), sc];
  }

  // `allProducers` is rebuilt from the UI's hardware list, so a producer
  // belonging to a component this page does not model would vanish with it.
  // Carry those across — but ONLY for ids that are still declared as
  // components, so deleting an arm or camera in the UI really does drop its
  // producer instead of resurrecting it from the original config.
  const preservedComponentIds = new Set(
    (hardware.components ?? [])
      .filter((c) => c.type !== 'trossen_base' && typeof c.id === 'string')
      .map((c) => c.id as string),
  );
  const emittedProducerIds = new Set(allProducers.map((p) => p.hardware_id));
  for (const p of originalConfig?.producers ?? []) {
    if (p.hardware_id && preservedComponentIds.has(p.hardware_id) && !emittedProducerIds.has(p.hardware_id)) {
      allProducers.push(p);
    }
  }

  // Preserve sections not managed by this page
  return {
    robot_name: originalConfig?.robot_name ?? system.name,
    hardware,
    producers: allProducers,
    // Rebuilt from the model, and pruned to arms that still exist. Passing the
    // original through verbatim is what left dangling pairs behind after an arm
    // was deleted, and what stopped re-added arms from ever getting one.
    teleop: buildTeleop(system, armsObj, originalConfig),
    backend: originalConfig?.backend ?? {},
    session: originalConfig?.session ?? {},
  };
}

export function ConfigurationPage() {
  const [selectedSystem, setSelectedSystem] = useState<string | null>(null);
  const [expandedHardware, setExpandedHardware] = useState<string[]>([]);
  const [showAddSystemModal, setShowAddSystemModal] = useState(false);
  const [showAddHardwareModal, setShowAddHardwareModal] = useState(false);
  const [showAddProducerModal, setShowAddProducerModal] = useState(false);
  const [showHardwareTypeModal, setShowHardwareTypeModal] = useState(false);
  const [hoveredSystem, setHoveredSystem] = useState<string | null>(null);
  // What's currently being edited in a modal — Hardware, Producer, or null.
  // The modals branch on shape, so a union is enough; callers narrow on use.
  const [editingItem, setEditingItem] = useState<Hardware | Producer | null>(null);
  const [editingHardwareId, setEditingHardwareId] = useState<string | null>(null);
  const [editingSystemId, setEditingSystemId] = useState<string | null>(null);
  const [selectedHardwareType, setSelectedHardwareType] = useState<'camera' | 'arm' | 'base'>('camera');
  const [selectedCameraType, setSelectedCameraType] = useState<'realsense_camera' | 'opencv_camera' | 'zed_camera'>('realsense_camera');
  const [currentParentHardwareId, setCurrentParentHardwareId] = useState<string | null>(null);
  const [hwFilter, setHwFilter] = useState<'all' | 'camera' | 'arm' | 'base'>('all');
  // Which button on which handle's cross is open for binding. Keyed by arm NAME
  // rather than id because that is what a session-control binding stores.
  const [glideButtonSel, setGlideButtonSel] = useState<{ arm: string; bit: number } | null>(null);

  // Systems loaded from the backend API
  const [systems, setSystems] = useState<HardwareSystem[]>([]);
  // Raw SDK configs keyed by system id, preserved for round-trip fidelity
  const [rawConfigs, setRawConfigs] = useState<Record<string, RawSdkConfig>>({});
  // Hardware status per system, hosted in a top-level context so the
  // gate on MonitorEpisodePage's Start button can read it without us
  // re-fetching or duplicating state. Local aliases keep the existing
  // call sites untouched.
  const {
    statuses: hwStatus,
    setStatus: setHwStatusEntry,
    clearStatus: clearHwStatus,
    testingSystemId,
    setTestingSystemId,
  } = useHwStatus();
  // Dirty flag: true when local edits have not yet been persisted
  const [hasUnsavedChanges, setHasUnsavedChanges] = useState(false);
  // Promise-based confirm for the discard action and the nav guard below.
  const { confirm, modalElement: confirmModalElement } = useConfirm();
  // In-app navigation guard: block client-side route changes while there are
  // unsaved edits (the data router in App.tsx makes useBlocker available).
  const blocker = useBlocker(
    ({ currentLocation, nextLocation }) =>
      hasUnsavedChanges && currentLocation.pathname !== nextLocation.pathname,
  );
  // Loading / saving indicators
  const [isLoading, setIsLoading] = useState(true);
  const [isSaving, setIsSaving] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  // Hardware-test state. `hwTesting` is an alias for the context's
  // `testingSystemId` so the per-card button styling can keep its
  // existing checks; the global flag is what locks page navigation in
  // the Header while a test is in flight.
  const hwTesting = testingSystemId;
  // Editing the system mid-test would change the very thing under
  // test or race with the SDK calls in flight, so every mutation
  // (save, reset to default, the Edit System pencil, add/edit/delete
  // hardware, add/edit/delete producer) gates on this flag.
  const mutationsLocked = hwTesting !== null;
  const lockedTitle = mutationsLocked
    ? 'Hardware test in progress — wait for it to finish'
    : '';
  // Shown on the disabled controls of hardware declared under
  // `hardware.components`, which this page displays but cannot yet edit.
  const viewOnlyTitle = 'Declared as a component — remove it from this system\'s config file to delete it';
  // `success: null` means the test is still in flight — the banner
  // renders in a cyan "Testing…" style and the output panel updates
  // line-by-line as SSE progress events arrive. `true` / `false` flip
  // it to the terminal pass / fail style on the final event.
  const [hwTestResult, setDryRunResult] = useState<{
    systemId: string;
    success: boolean | null;
    message: string;
    output: string[];
  } | null>(null);
  // Auto-scroll the output panel to the bottom whenever new lines
  // arrive, so the user always sees the most recent SDK output
  // without having to drag the scrollbar themselves.
  const outputPanelRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = outputPanelRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [hwTestResult?.output.length]);

  // App-level modal state (replaces native alert / confirm)
  const [appModal, setAppModal] = useState<{
    title: string;
    message: string;
    variant: 'danger' | 'warning' | 'info';
    confirmLabel?: string;
    onConfirm: () => void;
    onCancel?: () => void;
  } | null>(null);

  const showAlert = useCallback((message: string, title = 'Error') => {
    setAppModal({ title, message, variant: 'info', onConfirm: () => setAppModal(null) });
  }, []);

  const showConfirm = useCallback((message: string, onConfirm: () => void, title = 'Confirm', variant: 'danger' | 'warning' = 'danger') => {
    setAppModal({
      title,
      message,
      variant,
      confirmLabel: 'Confirm',
      onConfirm: () => { setAppModal(null); onConfirm(); },
      onCancel: () => setAppModal(null),
    });
  }, []);

  // -------------------------------------------------------------------------
  // Fetch systems from backend on mount
  // -------------------------------------------------------------------------
  useEffect(() => {
    setIsLoading(true);
    setLoadError(null);

    apiGet<RawSystemResponse[]>('/api/systems')
      .then((data) => {
        const converted = data.map((s) => sdkConfigToSystem(s.id, s));
        setSystems(converted);

        const configs: Record<string, RawSdkConfig> = {};
        data.forEach((s) => {
          configs[s.id] = s.config ?? {};
          // Seed the per-system Hardware status from the backend's
          // in-memory store. The backend keeps the most recent test
          // result for the lifetime of its process, so this re-hydrates
          // the badge after a page refresh; on a backend restart, every
          // system comes back with hw_status=null and the badge falls
          // back to Untested.
          if (s.hw_status) {
            setHwStatusEntry(s.id, {
              status: s.hw_status,
              message: s.hw_message ?? '',
            });
          }
        });
        setRawConfigs(configs);

        // Auto-select: use ?system= query param if present, else first system
        if (converted.length > 0) {
          const params = new URLSearchParams(window.location.search);
          const fromParam = params.get('system');
          setSelectedSystem(prev => prev ?? (fromParam && converted.some(s => s.id === fromParam) ? fromParam : converted[0].id));
        }
      })
      .catch((err) => {
        setLoadError(describeError(err));
      })
      .finally(() => {
        setIsLoading(false);
      });
    // setHwStatusEntry is stable (useCallback in HwStatusContext), so
    // including it doesn't actually retrigger the fetch.
  }, [setHwStatusEntry]);

  // Warn before unload while a Hardware Test is mid-flight. The Header
  // already blocks in-app navigation, but the user can still close
  // the tab or reload the page; the browser shows its generic
  // "Leave site?" prompt when preventDefault + returnValue are set.
  useEffect(() => {
    // Warn on tab-close / hard-reload either while a test is running OR while
    // there are unsaved config edits (useBlocker only catches in-app nav).
    if (testingSystemId === null && !hasUnsavedChanges) return;
    const handler = (e: BeforeUnloadEvent) => {
      e.preventDefault();
      e.returnValue = '';
    };
    window.addEventListener('beforeunload', handler);
    return () => window.removeEventListener('beforeunload', handler);
  }, [testingSystemId, hasUnsavedChanges]);

  // When the nav guard blocks a route change, ask the operator and either
  // proceed (discard) or reset (stay). Reuses the promise-based confirm.
  useEffect(() => {
    if (blocker.state !== 'blocked') return;
    confirm({
      title: 'Unsaved changes',
      message: 'You have unsaved configuration changes. Leave without saving?',
      confirmLabel: 'Leave',
      variant: 'warning',
    }).then(ok => (ok ? blocker.proceed() : blocker.reset()));
  }, [blocker, confirm]);

  // -------------------------------------------------------------------------
  // Save current system config to backend
  // -------------------------------------------------------------------------
  const handleSave = useCallback(async () => {
    if (!selectedSystem) return;
    const system = systems.find(s => s.id === selectedSystem);
    if (!system) return;

    setIsSaving(true);
    try {
      const config = systemToSdkConfig(system, rawConfigs[selectedSystem] ?? {});
      await apiPut(`/api/systems/${selectedSystem}`, config);

      // Update the raw config cache so subsequent saves remain correct
      setRawConfigs(prev => ({ ...prev, [selectedSystem]: config }));
      // A passing test on the previous config doesn't validate the new
      // one — drop the cached status so the badge falls back to
      // "Untested" and the Start button on Monitor re-engages its gate.
      clearHwStatus(selectedSystem);
      setHasUnsavedChanges(false);
      toast.success('Configuration saved');
    } catch (err) {
      const msg = describeError(err);
      showAlert(`Failed to save configuration: ${msg}`);
      toast.error(`Save failed: ${msg}`);
    } finally {
      setIsSaving(false);
    }
  }, [selectedSystem, systems, rawConfigs, showAlert, clearHwStatus]);

  const handleResetToDefault = useCallback(() => {
    if (!selectedSystem || !RESETTABLE_SYSTEMS.includes(selectedSystem)) return;

    showConfirm(
      'This will reset the entire configuration for this system back to its factory default. All customisations will be lost.',
      async () => {
        try {
          const data = await apiPost<RawSystemResponse>(`/api/systems/${selectedSystem}/reset`);
          const restored = sdkConfigToSystem(data.id, data);
          setSystems(prev => prev.map(s => s.id === data.id ? restored : s));
          setRawConfigs(prev => ({ ...prev, [data.id]: data.config ?? {} }));
          setHasUnsavedChanges(false);
          toast.success('Configuration reset to default');
        } catch (err) {
          showAlert(`Failed to reset configuration: ${describeError(err)}`);
        }
      },
      'Reset to Default',
      'warning',
    );
  }, [selectedSystem, showConfirm, showAlert]);

  // Discard unsaved edits by re-fetching the saved config from the backend —
  // a clean revert (names, hardware, producers) without tracking per-field
  // baselines. There is otherwise no way to abandon edits short of a reload.
  const handleDiscardChanges = useCallback(async () => {
    const ok = await confirm({
      title: 'Discard changes?',
      message: 'Discard all unsaved changes to this configuration?',
      confirmLabel: 'Discard',
      variant: 'warning',
    });
    if (!ok) return;
    try {
      const data = await apiGet<RawSystemResponse[]>('/api/systems');
      setSystems(data.map(s => sdkConfigToSystem(s.id, s)));
      const configs: Record<string, RawSdkConfig> = {};
      data.forEach(s => { configs[s.id] = s.config ?? {}; });
      setRawConfigs(configs);
      setHasUnsavedChanges(false);
      toast.success('Changes discarded');
    } catch (err) {
      showAlert(`Failed to reload configuration: ${describeError(err)}`);
    }
  }, [confirm, showAlert]);

  // Run the hardware test for a system. Extracted from the card button so it
  // can also be triggered by the `?autotest=1` deep link below. Streams SSE
  // progress into the banner; single-flight via the global testingSystemId.
  const runHardwareTest = useCallback(async (systemId: string) => {
    if (testingSystemId !== null) return;
    // Select the system being tested so the highlighted card always matches
    // the result banner below it. Without this, a card's TEST button (which
    // stops propagation to avoid toggling selection) leaves a different card
    // selected while the banner reports this system's verdict — e.g. the
    // Stationary card highlighted while the banner reads "FAILED — Solo".
    setSelectedSystem(systemId);
    setTestingSystemId(systemId);
    setDryRunResult({ systemId, success: null, message: 'Running hardware test…', output: [] });
    const controller = new AbortController();
    // Last-resort net in case the backend hangs without ever sending a
    // terminal event. The backend owns the real budget — `compute_bringup_budget`
    // in app/hw_test.py, which scales with device count and is capped at 300s
    // (a depth-enabled ZED costs ~30s to open, so a 3-camera rig needs far more
    // than the 90s this once assumed).
    //
    // Keep this in step with the identical timer in `useHardwareTest.ts`: both
    // call POST /api/systems/{id}/test, so whichever one is lower is the real
    // limit. This copy is the one the card's TEST button uses.
    const safetyTimeoutId = window.setTimeout(() => controller.abort(), 330000);
    const collected: string[] = [];
    try {
      const res = await fetch(`/api/systems/${systemId}/test`, { method: 'POST', signal: controller.signal });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ detail: `Server returned ${res.status} ${res.statusText}` }));
        const detail = typeof err.detail === 'string' ? err.detail : `Server error ${res.status}`;
        throw new Error(detail);
      }
      const reader = res.body?.getReader();
      const decoder = new TextDecoder();
      if (!reader) throw new Error('No stream from server');
      let buffer = '';
      let finalised = false;
      while (!finalised) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';
        for (const line of lines) {
          if (!line.startsWith('data: ')) continue;
          try {
            const data = JSON.parse(line.slice(6));
            if (data.type === 'progress' && typeof data.message === 'string') {
              collected.push(data.message);
              setDryRunResult(prev => prev && prev.systemId === systemId ? { ...prev, output: [...collected] } : prev);
            } else if (data.type === 'complete') {
              setDryRunResult({ systemId, success: true, message: data.message, output: data.output || collected });
              setHwStatusEntry(systemId, { status: 'ready', message: data.message });
              // The test gates recording and the operator has often walked to
              // the robot during the ~15s run — confirm the pass audibly too.
              toast.success('Hardware test passed');
              announce('Hardware test passed');
              finalised = true;
              break;
            } else if (data.type === 'error') {
              setDryRunResult({ systemId, success: false, message: data.message, output: data.output || collected });
              setHwStatusEntry(systemId, { status: 'error', message: data.message });
              toast.error(`Hardware test failed: ${data.message}`);
              announce('Hardware test failed');
              finalised = true;
              break;
            }
          } catch {
            // Non-JSON SSE comment / keepalive — ignore.
          }
        }
      }
      if (!finalised) {
        const msg = 'Hardware test ended unexpectedly — the backend closed the connection before sending a result.';
        setDryRunResult({ systemId, success: false, message: msg, output: collected });
        setHwStatusEntry(systemId, { status: 'error', message: msg });
        toast.error(`Hardware test failed: ${msg}`);
        announce('Hardware test failed');
      }
    } catch (err) {
      const isTimeout = err instanceof DOMException && err.name === 'AbortError';
      const msg = isTimeout
        ? 'Hardware test stopped responding — the backend never returned a result. It may have crashed or be stuck on a hardware call. Last captured server output is below; try running the test again.'
        : describeError(err);
      setDryRunResult({ systemId, success: false, message: msg, output: collected });
      setHwStatusEntry(systemId, { status: 'error', message: msg });
      toast.error(`Hardware test failed: ${msg}`);
      announce('Hardware test failed');
    } finally {
      window.clearTimeout(safetyTimeoutId);
      setTestingSystemId(null);
    }
  }, [testingSystemId, setTestingSystemId, setDryRunResult, setHwStatusEntry, setSelectedSystem]);

  // Deep-link autotest: arriving at /configuration?system=<id>&autotest=1
  // (from the "Test Hardware" gate banners) auto-selects the system and starts
  // its test — turning a multi-click hunt into a single click. Latched so a
  // re-render can't re-fire; skipped if the system is already passing.
  const autotestFiredRef = useRef(false);
  useEffect(() => {
    if (autotestFiredRef.current) return;
    if (isLoading || loadError) return;
    const params = new URLSearchParams(window.location.search);
    if (params.get('autotest') !== '1') return;
    const sysId = params.get('system');
    if (!sysId || !systems.some(s => s.id === sysId)) return;
    if (hwStatus[sysId]?.status === 'ready') return; // already green — don't re-engage arms
    if (testingSystemId !== null) return;
    autotestFiredRef.current = true; // latch before the async call
    announce('Auto-starting hardware test — arms will engage');
    runHardwareTest(sysId);
  }, [isLoading, loadError, systems, hwStatus, testingSystemId, runHardwareTest]);

  const [systemForm, setSystemForm] = useState({
    name: '',
    description: ''
  });

  const [producerForm, setProducerForm] = useState({
    stream_id: '',
    poll_rate_hz: 30,
    timeout_ms: 3000,
    use_device_time: false,
    encoding: 'bgr8' as 'bgr8' | 'rgb8' | 'mono8'
  });

  const [cameraForm, setCameraForm] = useState({
    name: '',
    width: 1920,
    height: 1080,
    fps: 30,
    // RealSense
    serial_number: '',
    use_depth: true,
    // OpenCV
    device_index: '/dev/video0',
    backend: 'v4l2',
    warmup_frames: 10,
    // ZED. Width/height are derived from the resolution rather than typed,
    // because the ZED negotiates its frame size from this name alone.
    resolution: DEFAULT_ZED_RESOLUTION as ZedResolution,
    depth_mode: DEFAULT_ZED_DEPTH_MODE as ZedDepthMode
  });

  const [armForm, setArmForm] = useState({
    name: '',
    ip_address: '192.168.1.10',
    // Defaults must be values the driver actually knows: 'ViperX-300' /
    // 'Gripper' threw "Unknown model" at configure(), so an arm added from
    // the UI could never boot. Matches the default role ('leader') below.
    model: 'wxai_v0',
    end_effector: 'wxai_v0_leader',
    role: 'leader' as 'leader' | 'follower',
    paired_with: '',
    passive: false,
    // Wrist-roll offset side for a passive leader (±π/4 on J5). Mirror-mounted
    // left/right arms need opposite signs; chosen explicitly per arm.
    wristSide: 'left' as WristSide,
    // Leader gripper force feedback (off by default). Cubic constants from the
    // bilateral reference: leader N at full grip, follower N normalizer, offset.
    gripperFeedback: false,
    gripperFeedbackLeaderMax: 27,
    gripperFeedbackFollowerMax: 87.5,
    gripperFeedbackOffset: 8,
    // Per-joint operating limits. When disabled, nothing is emitted and the
    // controller's firmware defaults apply. When enabled, all four arrays
    // (length NUM_ARM_JOINTS) are pushed to the arm on every connect.
    limitsEnabled: false,
    positionMin: [...DEFAULT_JOINT_LIMITS.position_min],
    positionMax: [...DEFAULT_JOINT_LIMITS.position_max],
    velocityMax: [...DEFAULT_JOINT_LIMITS.velocity_max],
    effortMax: [...DEFAULT_JOINT_LIMITS.effort_max],
    tolerancesEnabled: false,
    positionTolerance: [...DEFAULT_JOINT_TOLERANCES.position_tolerance],
    velocityTolerance: [...DEFAULT_JOINT_TOLERANCES.velocity_tolerance],
    effortTolerance: [...DEFAULT_JOINT_TOLERANCES.effort_tolerance],
    positionToleranceSet: false,
    velocityToleranceSet: false,
    effortToleranceSet: false,
    // Per-joint clamp on outgoing teleop commands. Blank cell = that joint is
    // unclamped, which is the usual state for all but one or two axes.
    commandClampEnabled: false,
    commandClampMin: Array<number | null>(NUM_ARM_JOINTS).fill(null),
    commandClampMax: Array<number | null>(NUM_ARM_JOINTS).fill(null),
    // Goal time per teleop command. 0 = real-time, which is the SDK's own
    // default and what a mirror wants.
    writeMovingTimeS: 0,
    // One-Euro command smoothing. Defaults mirror the SDK's own
    // (TrossenArmComponent: min_cutoff 1.0 Hz, beta 0.9, d_cutoff 1.0 Hz) so
    // ticking the box on and saving reproduces the SDK default exactly.
    smoothingEnabled: false,
    smoothingGripper: false,
    smoothingMinCutoffHz: SMOOTHING_DEFAULTS.min_cutoff_hz,
    smoothingBeta: SMOOTHING_DEFAULTS.beta,
    smoothingDCutoffHz: SMOOTHING_DEFAULTS.d_cutoff_hz,
  });

  // Both bases share one form. Which half is shown is driven by the type of the
  // base being edited, never by the form itself — see `editingBaseType`.
  const [baseForm, setBaseForm] = useState({
    // SLATE (hardware.mobile_base)
    name: '',
    reset_odometry: false,
    enable_torque: true,
    // Rivet base (hardware.components, type trossen_base). Defaults mirror
    // TrossenBaseComponent's own so the form never shows a value the SDK
    // wouldn't have used anyway.
    max_linear_mps: 0.6,
    max_angular_rps: 1.2,
    max_lift_units_per_s: 8000,
    estop_battery_percent: 0,
    ready_timeout_s: 60,
  });

  // The type of the base currently open in the modal. A trossen_base can be
  // edited but not created here: creating one means adding a
  // `hardware.components` entry, and this page patches components rather than
  // authoring them. Null while adding, which is always a SLATE.
  const [editingBaseType, setEditingBaseType] = useState<BaseHardware['type'] | null>(null);

  const selectedSystemData = systems.find(s => s.id === selectedSystem);

  // Determine producer mode based on hardware type
  const getProducerMode = (hardwareType: string): 'poll' | 'push' => {
    // Push mode: realsense_camera, zed_camera
    if (hardwareType === 'realsense_camera' || hardwareType === 'zed_camera') {
      return 'push';
    }
    // Poll mode: trossen_arm, opencv_camera, slate_base, trossen_base, teleop_arm
    return 'poll';
  };

  const toggleHardwareExpand = (hardwareId: string) => {
    setExpandedHardware(prev =>
      prev.includes(hardwareId)
        ? prev.filter(id => id !== hardwareId)
        : [...prev, hardwareId]
    );
  };

  const openAddProducerModal = (hardwareId: string, _hardware: Hardware) => {
    setCurrentParentHardwareId(hardwareId);
    setEditingItem(null);
    setProducerForm({
      stream_id: '',
      poll_rate_hz: 30,
      timeout_ms: 3000,
      use_device_time: false,
      encoding: 'bgr8'
    });
    setShowAddProducerModal(true);
  };

  const openEditProducerModal = (hardwareId: string, producer: Producer) => {
    setCurrentParentHardwareId(hardwareId);
    setEditingItem(producer);
    setProducerForm({
      stream_id: producer.stream_id,
      poll_rate_hz: producer.poll_rate_hz || 30,
      timeout_ms: producer.timeout_ms || 3000,
      use_device_time: producer.use_device_time,
      encoding: producer.encoding || 'bgr8',
    });
    setShowAddProducerModal(true);
  };

  const handleAddProducer = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem || !currentParentHardwareId) return;

    // Validate unique stream_id
    const allProducers = selectedSystemData?.hardware.flatMap(h => h.producers) || [];
    if (allProducers.some(p => p.stream_id === producerForm.stream_id && (!editingItem || p.id !== editingItem.id))) {
      showAlert('stream_id must be unique across all producers in the system', 'Validation Error');
      return;
    }

    const hardware = selectedSystemData?.hardware.find(h => h.id === currentParentHardwareId);
    if (!hardware) return;

    const producerData: Producer = {
      id: editingItem ? editingItem.id : `prod-${Date.now()}`,
      type: hardware.type === 'trossen_arm'
        ? 'arm'
        : hardware.type === 'slate_base' || hardware.type === 'trossen_base'
          ? 'base'
          : 'camera',
      hardware_id: currentParentHardwareId,
      stream_id: producerForm.stream_id,
      mode: getProducerMode(hardware.type),
      poll_rate_hz: producerForm.poll_rate_hz,
      timeout_ms: producerForm.timeout_ms,
      use_device_time: producerForm.use_device_time,
      ...(hardware.type.includes('camera') && { encoding: producerForm.encoding })
    };

    setSystems(prev => prev.map(sys =>
      sys.id === selectedSystem
        ? {
            ...sys,
            hardware: sys.hardware.map(hw =>
              hw.id === currentParentHardwareId
                ? {
                    ...hw,
                    producers: editingItem
                      ? hw.producers.map(p => p.id === editingItem.id ? producerData : p)
                      : [...hw.producers, producerData]
                  }
                : hw
            )
          }
        : sys
    ));
    setHasUnsavedChanges(true);

    setShowAddProducerModal(false);
    setCurrentParentHardwareId(null);
  };

  const handleDeleteProducer = (hardwareId: string, producerId: string) => {
    showConfirm('Delete this producer?', () => {
      setSystems(prev => prev.map(sys =>
        sys.id === selectedSystem
          ? {
              ...sys,
              hardware: sys.hardware.map(hw =>
                hw.id === hardwareId
                  ? { ...hw, producers: hw.producers.filter(p => p.id !== producerId) }
                  : hw
              )
            }
          : sys
      ));
      setHasUnsavedChanges(true);
    }, 'Delete Producer');
  };

  const openAddHardwareModal = (type: 'camera' | 'arm' | 'base') => {
    setSelectedHardwareType(type);
    setEditingHardwareId(null);
    // Reset forms
    setCameraForm({
      name: '',
      width: 1920,
      height: 1080,
      fps: 30,
      serial_number: '',
      use_depth: true,
      device_index: '/dev/video0',
      backend: 'v4l2',
      warmup_frames: 10,
      resolution: DEFAULT_ZED_RESOLUTION,
      depth_mode: DEFAULT_ZED_DEPTH_MODE
    });
    setArmForm({
      name: '',
      ip_address: '192.168.1.10',
      model: 'wxai_v0',
      end_effector: 'wxai_v0_leader',
      role: 'leader',
      paired_with: '',
      passive: false,
      wristSide: 'left',
      gripperFeedback: false,
      gripperFeedbackLeaderMax: 27,
      gripperFeedbackFollowerMax: 87.5,
      gripperFeedbackOffset: 8,
      limitsEnabled: false,
      positionMin: [...DEFAULT_JOINT_LIMITS.position_min],
      positionMax: [...DEFAULT_JOINT_LIMITS.position_max],
      velocityMax: [...DEFAULT_JOINT_LIMITS.velocity_max],
      effortMax: [...DEFAULT_JOINT_LIMITS.effort_max],
      tolerancesEnabled: false,
      positionTolerance: [...DEFAULT_JOINT_TOLERANCES.position_tolerance],
      velocityTolerance: [...DEFAULT_JOINT_TOLERANCES.velocity_tolerance],
      effortTolerance: [...DEFAULT_JOINT_TOLERANCES.effort_tolerance],
      positionToleranceSet: false,
      velocityToleranceSet: false,
      effortToleranceSet: false,
      commandClampEnabled: false,
      commandClampMin: Array<number | null>(NUM_ARM_JOINTS).fill(null),
      commandClampMax: Array<number | null>(NUM_ARM_JOINTS).fill(null),
      writeMovingTimeS: 0,
      smoothingEnabled: false,
      smoothingGripper: false,
      smoothingMinCutoffHz: SMOOTHING_DEFAULTS.min_cutoff_hz,
      smoothingBeta: SMOOTHING_DEFAULTS.beta,
      smoothingDCutoffHz: SMOOTHING_DEFAULTS.d_cutoff_hz,
    });
    setBaseForm({
      name: '',
      reset_odometry: false,
      enable_torque: true,
      max_linear_mps: 0.6,
      max_angular_rps: 1.2,
      max_lift_units_per_s: 8000,
      estop_battery_percent: 0,
      ready_timeout_s: 60,
    });
    setEditingBaseType(null);
    setShowAddHardwareModal(true);
  };

  const openEditHardwareModal = (hardware: Hardware) => {
    setEditingHardwareId(hardware.id);

    if (hardware.type.includes('camera')) {
      const cam = hardware as CameraHardware;
      setSelectedHardwareType('camera');
      setSelectedCameraType(cam.type);
      setCameraForm({
        name: cam.name,
        width: cam.width,
        height: cam.height,
        fps: cam.fps,
        serial_number: cam.serial_number || '',
        use_depth: cam.use_depth || false,
        device_index: cam.device_index || '/dev/video0',
        backend: cam.backend || 'v4l2',
        warmup_frames: cam.warmup_frames || 10,
        resolution: cam.resolution || DEFAULT_ZED_RESOLUTION,
        depth_mode: cam.depth_mode || DEFAULT_ZED_DEPTH_MODE
      });
    } else if (hardware.type === 'trossen_arm') {
      const arm = hardware as ArmHardware;
      setSelectedHardwareType('arm');
      setArmForm({
        name: arm.name,
        ip_address: arm.ip_address,
        model: arm.model,
        end_effector: arm.end_effector,
        role: arm.role,
        paired_with: arm.paired_with || '',
        passive: arm.actuated === false,
        wristSide: wristSideFromOffsets(arm.joint_offsets),
        gripperFeedback: arm.gripper_force_feedback === true,
        gripperFeedbackLeaderMax: typeof arm.gripper_feedback_leader_max === 'number' ? arm.gripper_feedback_leader_max : 27,
        gripperFeedbackFollowerMax: typeof arm.gripper_feedback_follower_max === 'number' ? arm.gripper_feedback_follower_max : 87.5,
        gripperFeedbackOffset: typeof arm.gripper_feedback_offset === 'number' ? arm.gripper_feedback_offset : 8,
        limitsEnabled: !!(arm.position_min || arm.position_max || arm.velocity_max || arm.effort_max),
        positionMin: armLimitOrDefault(arm.position_min, 'position_min'),
        positionMax: armLimitOrDefault(arm.position_max, 'position_max'),
        velocityMax: armLimitOrDefault(arm.velocity_max, 'velocity_max'),
        effortMax: armLimitOrDefault(arm.effort_max, 'effort_max'),
        tolerancesEnabled: !!(arm.position_tolerance || arm.velocity_tolerance || arm.effort_tolerance),
        positionTolerance: armToleranceOrDefault(arm.position_tolerance, 'position_tolerance'),
        velocityTolerance: armToleranceOrDefault(arm.velocity_tolerance, 'velocity_tolerance'),
        effortTolerance: armToleranceOrDefault(arm.effort_tolerance, 'effort_tolerance'),
        // Only the columns the arm already declares. Ticking one on is an
        // explicit act, because an all-zero column is the tightest fault check
        // the controller can run, not an absence of one.
        positionToleranceSet: !!arm.position_tolerance,
        velocityToleranceSet: !!arm.velocity_tolerance,
        effortToleranceSet: !!arm.effort_tolerance,
        commandClampEnabled: !!(arm.command_position_min || arm.command_position_max),
        commandClampMin: clampColumnOrBlank(arm.command_position_min),
        commandClampMax: clampColumnOrBlank(arm.command_position_max),
        writeMovingTimeS: typeof arm.write_moving_time_s === 'number' ? arm.write_moving_time_s : 0,
        smoothingEnabled: arm.smoothing_enabled === true,
        smoothingGripper: arm.smoothing_gripper === true,
        smoothingMinCutoffHz: typeof arm.smoothing_min_cutoff_hz === 'number' ? arm.smoothing_min_cutoff_hz : SMOOTHING_DEFAULTS.min_cutoff_hz,
        smoothingBeta: typeof arm.smoothing_beta === 'number' ? arm.smoothing_beta : SMOOTHING_DEFAULTS.beta,
        smoothingDCutoffHz: typeof arm.smoothing_d_cutoff_hz === 'number' ? arm.smoothing_d_cutoff_hz : SMOOTHING_DEFAULTS.d_cutoff_hz,
      });
    } else if (hardware.type === 'slate_base' || hardware.type === 'trossen_base') {
      const base = hardware as BaseHardware;
      setSelectedHardwareType('base');
      setEditingBaseType(base.type);
      setBaseForm({
        name: base.name,
        reset_odometry: base.reset_odometry,
        enable_torque: base.enable_torque,
        // Fall back to the component's own defaults for any ceiling the config
        // leaves unset, so the form shows what the SDK would actually apply
        // rather than a zero the operator might save by accident.
        max_linear_mps: base.max_linear_mps ?? 0.6,
        max_angular_rps: base.max_angular_rps ?? 1.2,
        max_lift_units_per_s: base.max_lift_units_per_s ?? 8000,
        // Zero is the component's default and means "disabled", so it is the
        // correct fallback here — not a placeholder.
        estop_battery_percent: base.estop_battery_percent ?? 0,
        ready_timeout_s: base.ready_timeout_s ?? 60,
      });
    }

    setShowAddHardwareModal(true);
  };

  const handleAddSystem = async (e: React.FormEvent) => {
    e.preventDefault();

    if (editingSystemId) {
      // Edit existing system (local state, saved via Save button)
      setSystems(prev => prev.map(sys =>
        sys.id === editingSystemId
          ? { ...sys, name: systemForm.name, description: systemForm.description }
          : sys
      ));
      setHasUnsavedChanges(true);
    } else {
      // Create new system — persist to backend immediately
      const systemId = systemForm.name.toLowerCase().replace(/\s+/g, '_').replace(/[^a-z0-9_-]/g, '');
      try {
        const data = await apiPost<RawSystemResponse>('/api/systems', {
          id: systemId,
          name: systemForm.name,
        });
        const newSystem = sdkConfigToSystem(data.id, data);
        setSystems(prev => [...prev, newSystem]);
        setRawConfigs(prev => ({ ...prev, [data.id]: data.config ?? {} }));
        setSelectedSystem(data.id);
        toast.success(`Created system "${systemForm.name}"`);
      } catch (err) {
        showAlert(`Failed to create system: ${describeError(err)}`);
        return;
      }
    }

    setShowAddSystemModal(false);
    setEditingSystemId(null);
    setSystemForm({ name: '', description: '' });
  };

  const openEditSystemModal = (system: HardwareSystem, e: React.MouseEvent) => {
    e.stopPropagation(); // Prevent system selection when clicking edit
    setEditingSystemId(system.id);
    setSystemForm({
      name: system.name,
      description: system.description || ''
    });
    setShowAddSystemModal(true);
  };

  const handleAddCamera = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem) return;

    // A ZED's frame size comes from its named resolution, so take the
    // dimensions from the table rather than the width/height inputs, which the
    // form does not even show for a ZED. AUTO has no dimensions until the
    // camera is opened; keep whatever the form last held so the card shows
    // something rather than zeros.
    const zedDims =
      selectedCameraType === 'zed_camera' ? ZED_RESOLUTIONS[cameraForm.resolution] : null;

    const cameraData: CameraHardware = {
      id: editingHardwareId || `cam-${Date.now()}`,
      name: cameraForm.name,
      type: selectedCameraType,
      width: zedDims?.width ?? cameraForm.width,
      height: zedDims?.height ?? cameraForm.height,
      fps: cameraForm.fps,
      producers: [],
      ...(selectedCameraType === 'realsense_camera' && {
        serial_number: cameraForm.serial_number,
        use_depth: cameraForm.use_depth
      }),
      ...(selectedCameraType === 'opencv_camera' && {
        device_index: cameraForm.device_index,
        backend: cameraForm.backend,
        warmup_frames: cameraForm.warmup_frames
      }),
      ...(selectedCameraType === 'zed_camera' && {
        serial_number: cameraForm.serial_number,
        resolution: cameraForm.resolution,
        use_depth: cameraForm.use_depth,
        // Carried even when depth is off so toggling it back on does not lose
        // the operator's choice; the SDK ignores it while use_depth is false.
        depth_mode: cameraForm.depth_mode
      })
    };

    setSystems(prev => prev.map(sys => {
      if (sys.id !== selectedSystem) return sys;

      if (editingHardwareId) {
        // Edit existing
        return {
          ...sys,
          hardware: sys.hardware.map(hw =>
            hw.id === editingHardwareId
              ? { ...cameraData, producers: hw.producers }
              : hw
          )
        };
      } else {
        // Add new
        return { ...sys, hardware: [...sys.hardware, cameraData] };
      }
    }));
    setHasUnsavedChanges(true);

    setShowAddHardwareModal(false);
    setEditingHardwareId(null);
  };

  const handleAddArm = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem) return;

    // A passive (lightweight) leader carries actuated=false plus the affine
    // joint remap; only meaningful for a leader. Anything else is a normal arm.
    const isPassive = armForm.role === 'leader' && armForm.passive;
    const remap = isPassive ? lightweightLeaderRemap(armForm.wristSide) : undefined;

    // Gripper force feedback is a leader-only behavior (the leader's actuated
    // gripper renders the reflected force from the follower's grip effort).
    const isGripperFeedback = armForm.role === 'leader' && armForm.gripperFeedback;

    const armData: ArmHardware = {
      id: editingHardwareId || `arm-${Date.now()}`,
      name: armForm.name,
      type: 'trossen_arm',
      ip_address: armForm.ip_address,
      model: armForm.model,
      end_effector: armForm.end_effector,
      role: armForm.role,
      paired_with: armForm.paired_with || undefined,
      actuated: isPassive ? false : undefined,
      joint_signs: remap?.joint_signs,
      joint_offsets: remap?.joint_offsets,
      gripper_force_feedback: isGripperFeedback ? true : undefined,
      gripper_feedback_leader_max: isGripperFeedback ? armForm.gripperFeedbackLeaderMax : undefined,
      gripper_feedback_follower_max: isGripperFeedback ? armForm.gripperFeedbackFollowerMax : undefined,
      gripper_feedback_offset: isGripperFeedback ? armForm.gripperFeedbackOffset : undefined,
      position_min: armForm.limitsEnabled ? [...armForm.positionMin] : undefined,
      position_max: armForm.limitsEnabled ? [...armForm.positionMax] : undefined,
      velocity_max: armForm.limitsEnabled ? [...armForm.velocityMax] : undefined,
      effort_max: armForm.limitsEnabled ? [...armForm.effortMax] : undefined,
      // Per column, not per section: an arm that declares velocity and effort
      // tolerances but not position must keep position on the firmware default.
      // Emitting a zero column here would silently TIGHTEN the fault check on a
      // joint nobody asked to change.
      position_tolerance: armForm.tolerancesEnabled && armForm.positionToleranceSet ? [...armForm.positionTolerance] : undefined,
      velocity_tolerance: armForm.tolerancesEnabled && armForm.velocityToleranceSet ? [...armForm.velocityTolerance] : undefined,
      effort_tolerance: armForm.tolerancesEnabled && armForm.effortToleranceSet ? [...armForm.effortTolerance] : undefined,
      // Only when the section is on AND some joint is actually bounded, so
      // ticking the box and leaving every cell blank writes nothing.
      command_position_min:
        armForm.commandClampEnabled && armForm.commandClampMin.some((v) => v !== null)
          ? [...armForm.commandClampMin]
          : undefined,
      command_position_max:
        armForm.commandClampEnabled && armForm.commandClampMax.some((v) => v !== null)
          ? [...armForm.commandClampMax]
          : undefined,
      // Inert on a leader for the same reason smoothing is — nothing is
      // commanded to it — so it is not written back on one.
      write_moving_time_s: armForm.role !== 'leader' ? armForm.writeMovingTimeS : undefined,
      // A leader receives no commands, so its smoothing keys are inert; do not
      // write them back even if a hand-edited config carried them.
      smoothing_enabled: armForm.smoothingEnabled && armForm.role !== 'leader' ? true : undefined,
      smoothing_gripper: armForm.smoothingEnabled && armForm.smoothingGripper ? true : undefined,
      smoothing_min_cutoff_hz: armForm.smoothingEnabled ? armForm.smoothingMinCutoffHz : undefined,
      smoothing_beta: armForm.smoothingEnabled ? armForm.smoothingBeta : undefined,
      smoothing_d_cutoff_hz: armForm.smoothingEnabled ? armForm.smoothingDCutoffHz : undefined,
      producers: []
    };

    setSystems(prev => prev.map(sys => {
      if (sys.id !== selectedSystem) return sys;

      if (editingHardwareId) {
        // Edit existing
        return {
          ...sys,
          hardware: sys.hardware.map(hw =>
            hw.id === editingHardwareId
              ? { ...armData, producers: hw.producers }
              : hw
          )
        };
      } else {
        // Add new
        return { ...sys, hardware: [...sys.hardware, armData] };
      }
    }));
    setHasUnsavedChanges(true);

    setShowAddHardwareModal(false);
    setEditingHardwareId(null);
  };

  const handleAddBase = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem) return;

    // Mirror TrossenBaseComponent's own validation, which throws on a
    // non-positive limit or an out-of-range battery threshold. Catching it here
    // means a bad value never reaches a rig that has already energised its arms.
    if (editingBaseType === 'trossen_base') {
      const positive: [string, number][] = [
        ['Max Linear', baseForm.max_linear_mps],
        ['Max Angular', baseForm.max_angular_rps],
        ['Max Lift', baseForm.max_lift_units_per_s],
        ['Ready Timeout', baseForm.ready_timeout_s],
      ];
      const bad = positive.find(([, v]) => !Number.isFinite(v) || v <= 0);
      if (bad) {
        showAlert(`${bad[0]} must be greater than zero — the SDK refuses to start otherwise.`, 'Validation Error');
        return;
      }
      const battery = baseForm.estop_battery_percent;
      if (!Number.isFinite(battery) || battery < 0 || battery > 100) {
        showAlert('E-Stop Battery must be between 0 and 100 percent (0 disables the check).', 'Validation Error');
        return;
      }
    }

    setSystems(prev => prev.map(sys => {
      if (sys.id !== selectedSystem) return sys;

      if (editingHardwareId) {
        return {
          ...sys,
          hardware: sys.hardware.map(hw => {
            if (hw.id !== editingHardwareId) return hw;
            const existing = hw as BaseHardware;

            // Preserve the base's declared type. Rewriting a trossen_base as a
            // slate_base would move it out of hardware.components and drop
            // every ceiling below it, so the type is never taken from the form.
            if (existing.type === 'trossen_base') {
              return {
                ...existing,
                // Name is deliberately not editable: for a component-declared
                // base the name IS its component id, which this page patches by
                // rather than rewrites. Renaming here would change the label
                // without changing the config.
                max_linear_mps: baseForm.max_linear_mps,
                max_angular_rps: baseForm.max_angular_rps,
                max_lift_units_per_s: baseForm.max_lift_units_per_s,
                estop_battery_percent: baseForm.estop_battery_percent,
                ready_timeout_s: baseForm.ready_timeout_s,
              };
            }
            return {
              ...existing,
              name: baseForm.name,
              reset_odometry: baseForm.reset_odometry,
              enable_torque: baseForm.enable_torque,
            };
          })
        };
      }

      // Adding. Only the SLATE can be created from this page — a trossen_base
      // is a `hardware.components` entry, and systemToSdkConfig patches
      // existing components rather than authoring new ones.
      const baseData: BaseHardware = {
        id: `base-${Date.now()}`,
        name: baseForm.name,
        type: 'slate_base',
        reset_odometry: baseForm.reset_odometry,
        enable_torque: baseForm.enable_torque,
        producers: []
      };
      return { ...sys, hardware: [...sys.hardware, baseData] };
    }));
    setHasUnsavedChanges(true);

    setShowAddHardwareModal(false);
    setEditingHardwareId(null);
    setEditingBaseType(null);
  };

  const handleDeleteHardware = (hardwareId: string) => {
    showConfirm('Delete this hardware and all its producers?', () => {
      setSystems(prev => prev.map(sys =>
        sys.id === selectedSystem
          ? { ...sys, hardware: sys.hardware.filter(h => h.id !== hardwareId) }
          : sys
      ));
      setHasUnsavedChanges(true);
    }, 'Delete Hardware');
  };

  const getHardwareIcon = (hardware: Hardware) => {
    if (hardware.type.includes('camera')) return Camera;
    if (hardware.type === 'trossen_arm') return Bot;
    if (hardware.type === 'slate_base' || hardware.type === 'trossen_base') return Smartphone;
    return Server;
  };

  const renderCameraFields = (camera: CameraHardware) => {
    return (
      <div className="grid grid-cols-3 portrait:grid-cols-2 gap-[12px] text-[12px]">
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">Resolution</div>
          <div className="text-ink">
            {/* A ZED is configured by resolution NAME, so lead with the name and
                keep the pixels as the gloss. AUTO has no dimensions until the
                camera opens. */}
            {camera.type === 'zed_camera' ? (
              <>
                {camera.resolution ?? DEFAULT_ZED_RESOLUTION}
                {camera.resolution !== 'AUTO' && ` (${camera.width}x${camera.height})`}
                {` @ ${camera.fps}fps`}
              </>
            ) : (
              `${camera.width}x${camera.height} @ ${camera.fps}fps`
            )}
          </div>
        </div>
        {camera.type === 'realsense_camera' && (
          <>
            <div>
              <div className="text-dim text-[9px] uppercase mb-[4px]">Serial Number</div>
              <div className="text-ink">{camera.serial_number}</div>
            </div>
            <div>
              <div className="text-dim text-[9px] uppercase mb-[4px]">Depth Enabled</div>
              <div className="text-ink">{camera.use_depth ? 'Yes' : 'No'}</div>
            </div>
          </>
        )}
        {camera.type === 'opencv_camera' && (
          <>
            <div>
              <div className="text-dim text-[9px] uppercase mb-[4px]">Device Index</div>
              <div className="text-ink">{camera.device_index}</div>
            </div>
            <div>
              <div className="text-dim text-[9px] uppercase mb-[4px]">Backend / Warmup</div>
              <div className="text-ink">{camera.backend} / {camera.warmup_frames}f</div>
            </div>
          </>
        )}
        {camera.type === 'zed_camera' && (
          <>
            <div>
              <div className="text-dim text-[9px] uppercase mb-[4px]">Serial Number</div>
              <div className="text-ink">{camera.serial_number}</div>
            </div>
            <div>
              <div className="text-dim text-[9px] uppercase mb-[4px]">Depth</div>
              <div className="text-ink">
                {camera.use_depth
                  ? (camera.depth_mode ?? DEFAULT_ZED_DEPTH_MODE)
                  : 'Off'}
              </div>
            </div>
          </>
        )}
      </div>
    );
  };

  const renderArmFields = (arm: ArmHardware) => {
    return (
      <div className="grid grid-cols-4 portrait:grid-cols-2 gap-[12px] text-[12px]">
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">IP Address</div>
          <div className="text-ink">{arm.ip_address}</div>
        </div>
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">Model</div>
          <div className="text-ink">{arm.model}</div>
        </div>
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">End Effector</div>
          <div className="text-ink">{arm.end_effector}</div>
        </div>
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">Role</div>
          <div className={`capitalize ${arm.role === 'leader' ? 'text-brand' : 'text-ink'}`}>
            {arm.role}
          </div>
        </div>
        {arm.smoothing_enabled && (
          <div className="col-span-2">
            <div className="text-dim text-[9px] uppercase mb-[4px]">Command Smoothing</div>
            {/* A leader receives no commands, so the filter never runs on it.
                Say so rather than reporting a cutoff and beta that do nothing —
                and flag that the keys go away on the next save. */}
            <div className={arm.role === 'leader' ? 'text-dim' : 'text-ink'}>
              {`${arm.smoothing_min_cutoff_hz ?? SMOOTHING_DEFAULTS.min_cutoff_hz} Hz · β ${arm.smoothing_beta ?? SMOOTHING_DEFAULTS.beta}`}
              {arm.smoothing_gripper ? ' · incl. gripper' : ''}
              {arm.role === 'leader' && (
                <span className="text-yellow-500"> · ignored: a leader is not commanded</span>
              )}
            </div>
          </div>
        )}
        {/* A clamp changes where the arm can go, so it is worth a line — but
            only naming the joints actually bounded, since most are not. */}
        {(() => {
          const lo = arm.command_position_min;
          const hi = arm.command_position_max;
          if (!lo && !hi) return null;
          const bounded = ARM_JOINT_LABELS.map((label, i) => {
            const a = typeof lo?.[i] === 'number' ? lo[i] : null;
            const b = typeof hi?.[i] === 'number' ? hi[i] : null;
            if (a === null && b === null) return null;
            return `${label} ${a ?? '−∞'}…${b ?? '∞'}`;
          }).filter(Boolean);
          if (bounded.length === 0) return null;
          return (
            <div className="col-span-2">
              <div className="text-dim text-[9px] uppercase mb-[4px]">Command Clamp</div>
              <div className={arm.role === 'leader' ? 'text-dim' : 'text-ink'}>
                {bounded.join(' · ')}
                {arm.role === 'leader' && (
                  <span className="text-yellow-500"> · ignored: a leader is not commanded</span>
                )}
              </div>
            </div>
          );
        })()}
        {/* Only when non-zero: 0 is the default and the desired state, so
            reporting it on every follower card would be noise. A goal time is
            the one setting here that silently costs tracking, so it earns a
            line on the card when someone has turned it on. */}
        {typeof arm.write_moving_time_s === 'number' && arm.write_moving_time_s > 0 && (
          <div className="col-span-2">
            <div className="text-dim text-[9px] uppercase mb-[4px]">Command Goal Time</div>
            <div className={arm.role === 'leader' ? 'text-dim' : 'text-ink'}>
              {`${arm.write_moving_time_s}s`}
              {arm.role === 'leader' ? (
                <span className="text-yellow-500"> · ignored: a leader is not commanded</span>
              ) : (
                <span className="text-yellow-500"> · follower trails the leader by ~this much</span>
              )}
            </div>
          </div>
        )}
      </div>
    );
  };

  /**
   * The Glide handle's own controls, rendered inside the handle arm's card.
   *
   * These used to be one panel above the hardware list, which put a handle's
   * settings nowhere near the handle. Both underlying components address arms
   * by id — `glide_arm_input.arms` and each `glide_session_control` binding's
   * `arm_id` — so per-arm is what the config actually looks like, and it means
   * deleting a handle takes its bindings' UI with it instead of leaving rows
   * pointing at an arm that is gone.
   *
   * Only the two genuinely global knobs (poll rate, debounce) stay elsewhere.
   */
  /**
   * Whether this arm is a Glide handle, i.e. whether the handle controls make
   * sense on its card.
   *
   * The `glide_left` / `glide_right` models are what the shipped Glide presets
   * use, and they are the only arms with joysticks and buttons to read — an
   * ordinary `wxai_v0` leader has neither, so offering the controls there would
   * be a dead end. The two extra clauses catch a rig whose row has drifted from
   * its preset: if the config already reads this arm's inputs or binds one of
   * its buttons, the controls have to be reachable regardless of the model, or
   * that config becomes uneditable.
   */
  const isGlideHandle = (arm: ArmHardware): boolean => {
    if (arm.role !== 'leader') return false;
    if (arm.model.startsWith('glide')) return true;
    const sys = selectedSystemData;
    if (!sys) return false;
    return (
      !!sys.glideInputs?.arms.includes(arm.name) ||
      !!sys.sessionControl?.buttons.some((b) => b.arm_id === arm.name)
    );
  };

  const renderGlideHandle = (arm: ArmHardware) => {
    const sys = selectedSystemData;
    if (!sys) return null;
    const gi = sys.glideInputs;
    const sc = sys.sessionControl;

    const inputOn = !!gi?.arms.includes(arm.name);
    const bindings = sc?.buttons ?? [];
    const mine = bindings.filter((b) => b.arm_id === arm.name);
    const bindingFor = (bit: number) => mine.find((b) => b.bit === bit);
    // A hand-edited config can name a bit the hardware does not have. The cross
    // cannot show it, so list it separately rather than dropping it silently.
    const offCross = mine.filter((b) => !GLIDE_BUTTON_BITS.has(b.bit));

    const patchSys = (next: Partial<HardwareSystem>) =>
      setSystems((prev) => prev.map((x) => (x.id === sys.id ? { ...x, ...next } : x)));

    const toggleInput = (on: boolean) => patchSys(setGlideHandleInput(sys, arm.name, on));
    const setBinding = (bit: number, event: SessionControlEvent | null) =>
      patchSys(setGlideButtonBinding(sys, arm.name, bit, event));

    const selected =
      glideButtonSel?.arm === arm.name && GLIDE_BUTTON_BITS.has(glideButtonSel.bit)
        ? glideButtonSel.bit
        : null;

    return (
      <div className="mt-[10px] border-t border-surface pt-[10px]">
        <div className="flex items-center justify-between mb-[8px]">
          <div className="text-dim text-[9px] uppercase">Glide Handle</div>
          <label className="flex items-center gap-[6px] text-[11px] text-dim cursor-pointer">
            <input type="checkbox" checked={inputOn} onChange={(e) => toggleInput(e.target.checked)} />
            Read joystick &amp; buttons
          </label>
        </div>

        {!inputOn ? (
          <div className="text-dim text-[11px]">
            Off — this handle drives its follower, but its joystick and buttons do nothing.
          </div>
        ) : (
          <div className="flex flex-wrap items-start gap-[20px]">
            {/* The cross, laid out the way the buttons sit on the handle. */}
            <div className="grid grid-cols-3 grid-rows-3 gap-[4px] w-[210px] shrink-0">
              {GLIDE_BUTTON_LAYOUT.map(({ bit, label, cell }) => {
                const bound = bindingFor(bit);
                const isSel = selected === bit;
                return (
                  <button
                    key={bit}
                    type="button"
                    onClick={() => setGlideButtonSel(isSel ? null : { arm: arm.name, bit })}
                    title={`${label} button (bit ${bit})`}
                    className={`${cell} h-[46px] border px-[4px] flex flex-col items-center justify-center leading-tight transition-colors ${
                      isSel
                        ? 'border-brand bg-brand/10'
                        : bound
                          ? 'border-edge bg-surface hover:border-brand'
                          : 'border-dashed border-edge text-dim hover:border-brand'
                    }`}
                  >
                    <span className="text-[9px] uppercase text-dim">{label}</span>
                    <span className={`text-[10px] ${bound ? 'text-ink' : 'text-dim'}`}>
                      {bound ? SESSION_CONTROL_EVENT_SHORT[bound.event] : 'unbound'}
                    </span>
                  </button>
                );
              })}
              <div className="col-start-2 row-start-2 flex items-center justify-center text-dim text-[9px] uppercase">
                {arm.name}
              </div>
            </div>

            <div className="flex-1 min-w-[220px] space-y-[6px]">
              {selected === null ? (
                <div className="text-dim text-[11px]">
                  Pick a button to bind it. Positions match the physical handle;
                  the config stores each as its bit index.
                </div>
              ) : (
                <>
                  <div className="text-ink text-[11px]">
                    {GLIDE_BUTTON_LAYOUT.find((b) => b.bit === selected)?.label} button
                    <span className="text-dim"> · bit {selected}</span>
                  </div>
                  <select
                    value={bindingFor(selected)?.event ?? ''}
                    onChange={(e) =>
                      setBinding(selected, e.target.value ? (e.target.value as SessionControlEvent) : null)
                    }
                    className="w-full bg-app border border-edge text-ink px-[8px] py-[6px] text-[12px]"
                  >
                    <option value="">— not bound —</option>
                    {SESSION_CONTROL_EVENTS.map((ev) => (
                      <option key={ev} value={ev}>{SESSION_CONTROL_EVENT_LABELS[ev]}</option>
                    ))}
                  </select>
                </>
              )}
              {offCross.length > 0 && (
                <div className="text-[11px] text-yellow-500">
                  {offCross.map((b) => `bit ${b.bit} → ${SESSION_CONTROL_EVENT_SHORT[b.event]}`).join(', ')}
                  {' '}— this handle has four buttons (bits 0–3), so this never fires.
                  <button
                    type="button"
                    onClick={() =>
                      patchSys({
                        sessionControl: (() => {
                          const kept = bindings.filter(
                            (b) => !(b.arm_id === arm.name && !GLIDE_BUTTON_BITS.has(b.bit)),
                          );
                          return kept.length && sc ? { ...sc, buttons: kept } : undefined;
                        })(),
                      })
                    }
                    className="ml-[6px] underline hover:text-yellow-300"
                  >
                    remove
                  </button>
                </div>
              )}
            </div>
          </div>
        )}
      </div>
    );
  };

  const renderBaseFields = (base: BaseHardware) => {
    // The Rivet base (trossen_base) and the SLATE (slate_base) share nothing
    // but the word "base": the Rivet exposes per-axis ceilings including the
    // vertical lift, the SLATE exposes odometry and torque.
    if (base.type === 'trossen_base') {
      // The rail's ceiling is declared on the base AND on the glide_base leader
      // that drives it. They are applied in series, so a mismatch silently caps
      // the rail at the lower of the two — worth surfacing rather than leaving
      // an operator to wonder why raising one number changed nothing.
      const railOutOfSync =
        base.lift_leader_max != null &&
        base.max_lift_units_per_s != null &&
        base.lift_leader_max !== base.max_lift_units_per_s;

      return (
        <div className="flex flex-col gap-[12px]">
          {/* Drive */}
          <div>
            <div className="text-dim text-[9px] uppercase mb-[6px] tracking-wide">Drive</div>
            <div className="grid grid-cols-2 portrait:grid-cols-1 gap-[12px] text-[12px]">
              <div>
                <div className="text-dim text-[9px] uppercase mb-[4px]">Max Linear</div>
                <div className="text-ink">{base.max_linear_mps != null ? `${base.max_linear_mps} m/s` : '—'}</div>
              </div>
              <div>
                <div className="text-dim text-[9px] uppercase mb-[4px]">Max Angular</div>
                <div className="text-ink">{base.max_angular_rps != null ? `${base.max_angular_rps} rad/s` : '—'}</div>
              </div>
            </div>
          </div>

          {/* Linear rail — the base's vertical lift axis, surfaced on its own
              because its speed is tuned separately from the drive. */}
          <div className="border-t border-edge pt-[10px]">
            <div className="text-dim text-[9px] uppercase mb-[6px] tracking-wide">Linear Rail</div>
            <div className="grid grid-cols-2 portrait:grid-cols-1 gap-[12px] text-[12px]">
              <div>
                <div className="text-dim text-[9px] uppercase mb-[4px]">Max Speed</div>
                <div className="text-ink">{base.max_lift_units_per_s != null ? `${base.max_lift_units_per_s} units/s` : '—'}</div>
              </div>
              <div>
                <div className="text-dim text-[9px] uppercase mb-[4px]">Leader Axis Limit</div>
                <div className={railOutOfSync ? 'text-yellow-500' : 'text-ink'}>
                  {base.lift_leader_max != null ? `${base.lift_leader_max} units/s` : '—'}
                </div>
              </div>
            </div>
            {railOutOfSync && (
              <div className="mt-[8px] flex items-start gap-[6px] text-yellow-500 text-[11px]">
                <AlertTriangle className="w-[12px] h-[12px] shrink-0 mt-[2px]" />
                <span>
                  The leader scales the lift command before the base clamps it, so
                  the rail is capped at {Math.min(base.lift_leader_max!, base.max_lift_units_per_s!)} units/s.
                  Saving from the base editor sets both.
                </span>
              </div>
            )}
          </div>

          {/* Safety */}
          <div className="border-t border-edge pt-[10px]">
            <div className="text-dim text-[9px] uppercase mb-[6px] tracking-wide">Safety</div>
            <div className="grid grid-cols-2 portrait:grid-cols-1 gap-[12px] text-[12px]">
              <div>
                <div className="text-dim text-[9px] uppercase mb-[4px]">E-Stop Battery</div>
                <div className="text-ink">
                  {base.estop_battery_percent == null
                    ? '—'
                    : base.estop_battery_percent === 0
                      ? 'Disabled'
                      : `${base.estop_battery_percent} %`}
                </div>
              </div>
              <div>
                <div className="text-dim text-[9px] uppercase mb-[4px]">Ready Timeout</div>
                <div className="text-ink">{base.ready_timeout_s != null ? `${base.ready_timeout_s} s` : '—'}</div>
              </div>
            </div>
          </div>
        </div>
      );
    }
    return (
      <div className="grid grid-cols-2 gap-[12px] text-[12px]">
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">Reset Odometry</div>
          <div className="text-ink">{base.reset_odometry ? 'Enabled' : 'Disabled'}</div>
        </div>
        <div>
          <div className="text-dim text-[9px] uppercase mb-[4px]">Enable Torque</div>
          <div className="text-ink">{base.enable_torque ? 'Enabled' : 'Disabled'}</div>
        </div>
      </div>
    );
  };

  return (
    <div className="max-w-[1400px] mx-auto w-full px-4 sm:px-6 lg:px-[37px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif] h-full flex flex-col">
      {confirmModalElement}
      {/* Page Title */}
      <div className="mb-[35px]">
        <div className="flex flex-col gap-[7px]">
          <h1 className="text-[22px] text-ink capitalize leading-[22.4px]">Configuration</h1>
          <div className="h-[1px] bg-edge w-full" />
        </div>
      </div>

      {/* Unsaved-changes banner — sticky so it stays visible while the operator
          scrolls the hardware list, making "you must save" unmissable. */}
      {!isLoading && !loadError && hasUnsavedChanges && (
        <div className="sticky top-0 z-40 mb-[20px] flex items-center justify-between gap-3 border border-brand bg-brand/10 px-[16px] py-[10px]">
          <div className="flex items-center gap-[8px] text-brand text-[13px]">
            <AlertTriangle className="w-[16px] h-[16px] shrink-0" />
            <span>You have unsaved configuration changes — don't forget to save.</span>
          </div>
          <div className="flex items-center gap-[8px] shrink-0">
            <button
              onClick={handleDiscardChanges}
              disabled={isSaving || mutationsLocked}
              title={mutationsLocked ? lockedTitle : ''}
              className="border border-edge text-dim hover:border-white hover:text-ink px-[14px] py-[6px] text-[12px] uppercase disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Discard
            </button>
            <button
              onClick={handleSave}
              disabled={isSaving || mutationsLocked}
              title={mutationsLocked ? lockedTitle : ''}
              className="bg-brand text-white hover:bg-[#4aa8cc] px-[14px] py-[6px] text-[12px] uppercase flex items-center gap-[6px] disabled:opacity-50 disabled:cursor-not-allowed"
            >
              {isSaving ? <Loader2 className="w-[14px] h-[14px] animate-spin" /> : <Save className="w-[14px] h-[14px]" />}
              {isSaving ? 'Saving…' : 'Save Changes'}
            </button>
          </div>
        </div>
      )}

      {/* Loading state */}
      {isLoading && (
        <div className="flex-1 flex items-center justify-center">
          <div className="text-center">
            <Loader2 className="w-[32px] h-[32px] text-brand mx-auto mb-[16px] animate-spin" />
            <div className="text-dim text-[14px]">Loading hardware systems...</div>
          </div>
        </div>
      )}

      {/* Error state */}
      {loadError && !isLoading && (
        <div className="flex-1 flex items-center justify-center">
          <div className="text-center">
            <div className="text-red-500 text-[14px] mb-[8px]">{loadError}</div>
            <button
              onClick={() => window.location.reload()}
              className="text-brand text-[12px] underline hover:text-ink transition-colors"
            >
              Retry
            </button>
          </div>
        </div>
      )}

      {/* Level 1: Hardware System Selector */}
      {!isLoading && !loadError && (<>
      <div className="mb-[30px]">
        <div className="flex items-center justify-between mb-[16px]">
          <div className="flex items-center gap-[8px]">
            <Server className="w-[18px] h-[18px] text-brand" />
            <h2 className="text-[18px] text-ink uppercase">Hardware System</h2>
          </div>
          <div className="flex items-center gap-[8px]">
            {/* Save button — visible when there are unsaved changes */}
            {hasUnsavedChanges && (
              <button
                onClick={handleSave}
                disabled={isSaving || mutationsLocked}
                title={mutationsLocked ? lockedTitle : ''}
                className="bg-brand text-white px-[14px] py-[8px] flex items-center justify-center hover:bg-[#4aa8cc] disabled:opacity-50 disabled:cursor-not-allowed transition-colors text-[12px] uppercase"
              >
                {isSaving ? (
                  <Loader2 className="w-[14px] h-[14px] mr-[6px] animate-spin" />
                ) : (
                  <Save className="w-[14px] h-[14px] mr-[6px]" />
                )}
                {isSaving ? 'Saving...' : 'Save Changes'}
              </button>
            )}
            {/* Reset to Default — only for systems with a factory default */}
            {selectedSystem && RESETTABLE_SYSTEMS.includes(selectedSystem) && (
              <button
                onClick={handleResetToDefault}
                disabled={mutationsLocked}
                title={mutationsLocked ? lockedTitle : ''}
                className="bg-app border border-edge text-dim px-[14px] py-[8px] flex items-center justify-center hover:border-yellow-500 hover:text-yellow-500 disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:border-edge disabled:hover:text-dim transition-colors text-[12px] uppercase"
              >
                <RotateCcw className="w-[14px] h-[14px] mr-[6px]" />
                Reset to Default
              </button>
            )}
          </div>
        </div>

        {/* Status legend — the per-card badges use colour + a single word;
            spell out what each means so "Error" vs "Untested" isn't guessed. */}
        <div className="flex flex-wrap items-center gap-x-[16px] gap-y-[6px] mb-[12px] text-[10px] text-dim">
          <span className="flex items-center gap-[5px]"><span className="w-[6px] h-[6px] rounded-full bg-brand" />Ready — test passed, can record</span>
          <span className="flex items-center gap-[5px]"><span className="w-[6px] h-[6px] rounded-full bg-yellow-500" />Untested — run a Hardware Test first</span>
          <span className="flex items-center gap-[5px]"><span className="w-[6px] h-[6px] rounded-full bg-red-500" />Error — last test failed</span>
          <span className="flex items-center gap-[5px]"><span className="w-[6px] h-[6px] rounded-full bg-green-500" />Active — recording now</span>
        </div>

        <div className="grid grid-cols-4 portrait:grid-cols-2 gap-[12px]">
          {[...systems].sort((a, b) => {
            // Shipped layouts first, smallest to largest; anything user-created
            // sorts after them.
            const order: Record<string, number> = { solo: 0, solo_glide: 1, stationary: 2, mobile: 3, workbench: 4, rivet: 5 };
            return (order[a.id] ?? 99) - (order[b.id] ?? 99);
          }).map(system => {
            const isConfigured = system.hardware.length > 0;
            const hasProducers = system.hardware.some(hw => hw.producers.length > 0);
            const sysHwStatus = hwStatus[system.id]?.status || 'unknown';

            // Status priority: active > ready > error > untested. Layout
            // problems (no hardware, hardware without producers) used to
            // show an "Incomplete" badge here but were unreliable and
            // duplicated the per-hardware warnings already rendered in
            // the detail pane, so the badge is simply omitted in those
            // cases now.
            //
            // "Untested" means the system is configured (has hardware
            // and producers) but no Test has been run yet — the Start
            // Session button on MonitorEpisodePage gates on the Ready
            // status, so users have to Test before recording.
            let badgeLabel = '';
            let badgeColor = '';
            let dotColor = '';

            if (sysHwStatus === 'active') {
              badgeLabel = 'Active';
              badgeColor = 'bg-green-500/20 border-green-500 text-green-500';
              dotColor = 'bg-green-500';
            } else if (sysHwStatus === 'ready') {
              badgeLabel = 'Ready';
              badgeColor = 'bg-brand/20 border-brand text-brand';
              dotColor = 'bg-brand';
            } else if (sysHwStatus === 'error') {
              badgeLabel = 'Error';
              badgeColor = 'bg-red-500/20 border-red-500 text-red-500';
              dotColor = 'bg-red-500';
            } else if (isConfigured && hasProducers) {
              badgeLabel = 'Untested';
              badgeColor = 'bg-yellow-500/20 border-yellow-500 text-yellow-500';
              dotColor = 'bg-yellow-500';
            }

            return (
              <div
                key={system.id}
                role="button"
                tabIndex={0}
                aria-pressed={selectedSystem === system.id}
                onClick={() => setSelectedSystem(system.id)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); setSelectedSystem(system.id); }
                }}
                className={`p-[16px] border transition-all text-left relative cursor-pointer focus:outline-none focus-visible:ring-2 focus-visible:ring-brand ${
                  // Errored systems are tinted red across the whole card so they
                  // can't be missed at a glance (TDS-160) — red is in addition to
                  // the Error badge + text, never the only signal.
                  sysHwStatus === 'error'
                    ? selectedSystem === system.id
                      ? 'bg-red-500/15 border-red-500 border-2'
                      : 'bg-red-500/10 border-red-500 hover:border-red-400'
                    : selectedSystem === system.id
                      ? 'bg-edge border-brand border-2'
                      : 'bg-surface border-edge hover:border-dim'
                }`}
              >
                {badgeLabel && (
                  <div className={`absolute top-[8px] right-[8px] flex items-center gap-[4px] border px-[6px] py-[2px] text-[8px] font-bold uppercase ${badgeColor}`}>
                    <div className={`w-[5px] h-[5px] rounded-full ${dotColor}`} />
                    {badgeLabel}
                  </div>
                )}

                <div className="text-ink text-[14px] font-bold mb-[6px] truncate pr-[50px]">{system.name}</div>
                <div className="text-dim text-[11px] mb-[8px] line-clamp-2 min-h-[32px]">{system.description}</div>
                <div className="flex items-center justify-between mt-[8px]">
                  <div className="text-brand text-[10px]">{system.hardware.length} devices</div>
                  <div className="flex items-center gap-[4px]">
                    <button
                      onClick={(e) => { e.stopPropagation(); runHardwareTest(system.id); }}
                      disabled={hwTesting !== null}
                      className={`px-[12px] py-[6px] text-[11px] font-bold uppercase transition-colors rounded flex items-center gap-[5px] ${
                        hwTesting === system.id
                          ? 'bg-brand/30 text-brand cursor-wait'
                          : hwTesting !== null
                            ? 'bg-brand/40 text-ink/60 cursor-not-allowed'
                            : sysHwStatus === 'ready'
                              // Already passing — quieter style so the
                              // button doesn't compete with the Ready
                              // badge, but still clickable for re-tests.
                              ? 'bg-transparent border border-edge text-dim hover:border-brand hover:text-ink'
                              : 'bg-brand hover:bg-[#4aa8cc] text-white'
                      }`}
                      title={sysHwStatus === 'ready' ? 'Re-test hardware connectivity' : 'Test hardware connectivity'}
                    >
                      <Radio className="w-[12px] h-[12px]" />
                      {hwTesting === system.id ? 'Testing…' : sysHwStatus === 'ready' ? 'Re-test' : 'Test'}
                    </button>
                    <button
                      onClick={(e) => openEditSystemModal(system, e)}
                      disabled={mutationsLocked}
                      aria-label={`Edit ${system.name}`}
                      title={mutationsLocked ? lockedTitle : 'Edit system'}
                      className="p-[4px] hover:bg-brand bg-app disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-app transition-colors rounded"
                    >
                      <Edit className="w-[12px] h-[12px] text-dim hover:text-ink" />
                    </button>
                  </div>
                </div>
              </div>
            );
          })}
        </div>

        {/* Dry Run Result Banner */}
        {hwTestResult && (() => {
          const inProgress = hwTestResult.success === null;
          const passed = hwTestResult.success === true;
          const resultSystemName =
            systems.find(s => s.id === hwTestResult.systemId)?.name ?? hwTestResult.systemId;
          // Three-state styling: cyan while running, green on pass,
          // red on fail. Same colour family as the badges so the
          // banner matches the system card's verdict at a glance.
          const palette = inProgress
            ? { bg: 'bg-brand/10', border: 'border-brand', text: 'text-brand' }
            : passed
              ? { bg: 'bg-green-500/10', border: 'border-green-500', text: 'text-green-500' }
              : { bg: 'bg-red-500/10', border: 'border-red-500', text: 'text-red-500' };
          const heading = inProgress
            ? 'Testing Hardware'
            : passed
              ? 'Hardware Test Passed'
              : 'Hardware Test Failed';
          return (
            <div className={`mt-[12px] p-[12px] border rounded ${palette.bg} ${palette.border}`}>
              <div className="flex items-center justify-between mb-[6px]">
                <span className={`text-[12px] font-bold uppercase flex items-center gap-[8px] ${palette.text}`}>
                  {inProgress && <Loader2 className="w-[14px] h-[14px] animate-spin" />}
                  {heading} — {resultSystemName}
                </span>
                {/* Action row hidden while the test is running so the user
                    can't dismiss/retry a banner that's still streaming —
                    same lock policy as the nav bar. On failure, offer a
                    one-click Retry so the next step is obvious instead of
                    hunting for the card's TEST button again. */}
                {!inProgress && (
                  <div className="flex items-center gap-[10px]">
                    {!passed && (
                      <button
                        onClick={() => runHardwareTest(hwTestResult.systemId)}
                        disabled={testingSystemId !== null}
                        className="flex items-center gap-[5px] border border-red-500 text-red-400 hover:bg-red-500 hover:text-white px-[10px] py-[4px] text-[11px] font-bold uppercase rounded transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
                      >
                        <RotateCcw className="w-[12px] h-[12px]" />
                        Retry test
                      </button>
                    )}
                    <button onClick={() => setDryRunResult(null)} className="text-dim hover:text-ink text-[16px]">x</button>
                  </div>
                )}
              </div>
              <div className="text-ink text-[11px] mb-[6px]">{hwTestResult.message}</div>
              {/* Output panel only after the test finishes — during
                  the run the user just sees the spinner + status. We
                  still accumulate progress events live so the panel
                  has the full log to render on completion. */}
              {!inProgress && (
                <>
                  <div className="text-dim text-[10px] uppercase mb-[6px]">
                    Output ({hwTestResult.output.length} {hwTestResult.output.length === 1 ? 'line' : 'lines'})
                  </div>
                  <div
                    ref={outputPanelRef}
                    className="bg-app border border-edge p-[8px] rounded h-[400px] overflow-y-auto font-mono text-[10px] text-dim whitespace-pre-wrap"
                  >
                    {hwTestResult.output.length === 0 ? (
                      <div className="text-dim">No output captured.</div>
                    ) : (
                      hwTestResult.output.map((line, i) => (
                        <div
                          key={i}
                          className={
                            line.toLowerCase().includes('[critical]') || line.toLowerCase().includes('[error]')
                              ? 'text-red-400'
                              : ''
                          }
                        >
                          {line}
                        </div>
                      ))
                    )}
                  </div>
                </>
              )}
            </div>
          );
        })()}
      </div>

      {/* Level 2: Hardware Cards */}
      {selectedSystemData && (
        <div className="flex-1 flex flex-col">
          {/* System Header with Hover Tooltip */}
          <div className="mb-[16px]">
            <div
              className="inline-block relative"
              onMouseEnter={() => setHoveredSystem(selectedSystemData.id)}
              onMouseLeave={() => setHoveredSystem(null)}
            >
              <div className="text-dim text-[9px] uppercase mb-[4px]">Hardware System</div>
              <h2 className="text-[16px] text-ink cursor-help border-b border-dashed border-brand/50 inline-block">{selectedSystemData.name}</h2>

              {/* Hover Tooltip */}
              {hoveredSystem === selectedSystemData.id && (
                <div className="absolute top-full left-0 mt-[8px] bg-edge border-2 border-brand p-[16px] w-[500px] max-w-[90vw] z-[100] shadow-2xl">
                  <div className="text-ink text-[12px] font-bold mb-[8px]">{selectedSystemData.name}</div>
                  {selectedSystemData.description && (
                    <div className="text-dim text-[11px] mb-[12px]">{selectedSystemData.description}</div>
                  )}

                  <div className="border-t border-surface pt-[12px] space-y-[8px]">
                    <div className="text-brand text-[10px] uppercase font-bold mb-[8px]">Hardware Overview</div>

                    {selectedSystemData.hardware.length === 0 ? (
                      <div className="text-dim text-[11px]">No hardware configured</div>
                    ) : (
                      <div className="space-y-[6px]">
                        {selectedSystemData.hardware.map(hw => (
                          <div key={hw.id} className="flex items-center justify-between text-[11px] bg-surface p-[8px]">
                            <div className="flex items-center gap-[8px]">
                              <div className="text-ink font-mono">{hw.name}</div>
                              <div className="text-dim">({hw.type.replace('_', ' ')})</div>
                            </div>
                            <div className="text-brand">{hw.producers.length} producer{hw.producers.length !== 1 ? 's' : ''}</div>
                          </div>
                        ))}
                      </div>
                    )}

                    <div className="border-t border-surface pt-[8px] mt-[8px] flex items-center justify-between text-[10px]">
                      <div className="text-dim">Total</div>
                      <div className="text-ink">
                        {selectedSystemData.hardware.length} device{selectedSystemData.hardware.length !== 1 ? 's' : ''}, {' '}
                        {selectedSystemData.hardware.reduce((sum, hw) => sum + hw.producers.length, 0)} producer{selectedSystemData.hardware.reduce((sum, hw) => sum + hw.producers.length, 0) !== 1 ? 's' : ''}
                      </div>
                    </div>
                  </div>
                </div>
              )}
            </div>
          </div>

          {/* Layout Warning Banner — only shown when counts deviate from the expected layout */}
          {(() => {
            const layoutSpecs: Record<string, { label: string; leaders: number; followers: number; cameras: number; bases: number }> = {
              solo:                { label: 'Solo',                leaders: 1, followers: 1, cameras: 2, bases: 0 },
              solo_glide:          { label: 'Solo Glide',          leaders: 1, followers: 1, cameras: 2, bases: 0 },
              stationary:          { label: 'Stationary',          leaders: 2, followers: 2, cameras: 4, bases: 0 },
              mobile:              { label: 'Mobile',              leaders: 2, followers: 2, cameras: 3, bases: 1 },
              workbench:           { label: 'Workbench',           leaders: 2, followers: 2, cameras: 3, bases: 0 },
              // Three ZEDs: main plus the two side cameras, now fitted.
              rivet:               { label: 'Rivet',               leaders: 2, followers: 2, cameras: 3, bases: 1 },
            };
            const spec = layoutSpecs[selectedSystemData.id];
            if (!spec) return null;

            const hw = selectedSystemData.hardware;
            const arms = hw.filter(h => h.type === 'trossen_arm');
            const cameras = hw.filter(h => h.type.includes('camera'));
            const bases = hw.filter(h => h.type === 'slate_base' || h.type === 'trossen_base');

            const actualLeaders = arms.filter(a => (a as ArmHardware).role === 'leader').length;
            const actualFollowers = arms.filter(a => (a as ArmHardware).role === 'follower').length;
            const actualCameras = cameras.length;
            const actualBases = bases.length;

            // Each arm must have exactly 1 producer
            const armProducerIssues = arms.filter(a => a.producers.length !== 1);
            // Each base must have exactly 1 producer
            const baseProducerIssues = bases.filter(b => b.producers.length !== 1);
            // Each camera must have 1 producer (+ optionally 1 depth producer, so 1 or 2)
            const cameraProducerIssues = cameras.filter(c => c.producers.length < 1 || c.producers.length > 2);

            const leadersOff = actualLeaders !== spec.leaders;
            const followersOff = actualFollowers !== spec.followers;
            const camerasOff = actualCameras !== spec.cameras;
            const basesOff = actualBases !== spec.bases;
            const producersOff = armProducerIssues.length > 0 || baseProducerIssues.length > 0 || cameraProducerIssues.length > 0;

            if (!leadersOff && !followersOff && !camerasOff && !basesOff && !producersOff) return null;

            const issues: string[] = [];
            if (leadersOff) issues.push(`${spec.leaders} leader${spec.leaders !== 1 ? 's' : ''} (currently ${actualLeaders})`);
            if (followersOff) issues.push(`${spec.followers} follower${spec.followers !== 1 ? 's' : ''} (currently ${actualFollowers})`);
            if (camerasOff) issues.push(`${spec.cameras} camera${spec.cameras !== 1 ? 's' : ''} (currently ${actualCameras})`);
            if (basesOff) issues.push(`${spec.bases} mobile base${spec.bases !== 1 ? 's' : ''} (currently ${actualBases})`);
            if (armProducerIssues.length > 0) issues.push(`1 producer per arm — ${armProducerIssues.map(a => `${a.name} has ${a.producers.length}`).join(', ')}`);
            if (baseProducerIssues.length > 0) issues.push(`1 producer per base — ${baseProducerIssues.map(b => `${b.name} has ${b.producers.length}`).join(', ')}`);
            if (cameraProducerIssues.length > 0) issues.push(`1–2 producers per camera (1 + optional depth) — ${cameraProducerIssues.map(c => `${c.name} has ${c.producers.length}`).join(', ')}`);

            return (
              <div className="mb-[16px] p-[12px] border border-yellow-500/40 bg-yellow-500/5 flex items-start gap-[10px]">
                <AlertTriangle className="w-[16px] h-[16px] text-yellow-500 shrink-0 mt-[2px]" />
                <div className="text-[11px] text-dim leading-[1.5]">
                  <span className="text-yellow-500 font-bold uppercase">Warning:</span>{' '}
                  The <span className="text-ink">{spec.label}</span> layout expects:
                  <ul className="mt-[4px] ml-[12px] list-disc space-y-[2px]">
                    {issues.map((issue, i) => (
                      <li key={i} className="text-yellow-500">{issue}</li>
                    ))}
                  </ul>
                  <div className="mt-[6px]">
                    This system no longer adheres to the expected robot layout.
                    {' '}Only make changes if you understand the consequences.
                  </div>
                </div>
              </div>
            );
          })()}


          <div className="flex items-center justify-between mb-[16px]">
            <div className="flex items-center gap-[12px]">
              <h2 className="text-[16px] text-ink uppercase">Hardware Devices</h2>
              <div className="flex items-center gap-[4px] bg-edge p-[3px] rounded">
                {(['all', 'camera', 'arm', 'base'] as const).map(f => (
                  <button key={f} onClick={() => setHwFilter(f)}
                    className={`px-[8px] py-[3px] text-[9px] uppercase rounded transition-colors ${
                      hwFilter === f ? 'bg-brand text-white' : 'text-dim hover:text-white'
                    }`}>
                    {f === 'all' ? 'All' : f === 'camera' ? 'Cameras' : f === 'arm' ? 'Arms' : 'Base'}
                  </button>
                ))}
              </div>
            </div>
            <button
              onClick={() => setShowHardwareTypeModal(true)}
              disabled={mutationsLocked}
              title={mutationsLocked ? lockedTitle : ''}
              className="bg-brand text-white px-[12px] py-[6px] text-[11px] uppercase hover:bg-[#4aa8cc] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-brand transition-colors flex items-center gap-[6px]"
            >
              <Plus className="w-[12px] h-[12px]" />
              Add Hardware
            </button>
          </div>

          <div className="flex-1 bg-surface border border-edge overflow-auto">
            <div className="p-[12px] space-y-[6px]">
              {selectedSystemData.hardware.filter(hw => {
                if (hwFilter === 'all') return true;
                if (hwFilter === 'camera') return hw.type.includes('camera');
                if (hwFilter === 'arm') return hw.type === 'trossen_arm';
                if (hwFilter === 'base') return hw.type === 'slate_base' || hw.type === 'trossen_base';
                return true;
              }).map(hardware => {
                const HardwareIcon = getHardwareIcon(hardware);
                const isExpanded = expandedHardware.includes(hardware.id);
                const hasProducers = hardware.producers.length > 0;
                // A component-declared base can be edited (the base form patches
                // its fields in place) but not deleted: systemToSdkConfig patches
                // hardware.components and never rebuilds it, so a delete here
                // would drop the card while leaving the component in the config.
                const undeletableHardware = hardware.type === 'trossen_base';

                // Color tints per hardware type
                const tintClass = hardware.type.includes('camera')
                  ? 'border-l-brand/40 bg-brand/[0.02]'
                  : hardware.type === 'trossen_arm'
                    ? 'border-l-green-500/40 bg-green-500/[0.02]'
                    : 'border-l-orange-500/40 bg-orange-500/[0.02]';

                return (
                  <div key={hardware.id} className={`border border-l-[3px] ${tintClass} ${hasProducers ? 'border-edge' : 'border-yellow-600/50'}`}>
                    {/* Hardware Header — name + actions on one line */}
                    <div className="px-[12px] py-[10px]">
                      <div className="flex items-center justify-between">
                        <div className="flex items-center gap-[10px]">
                          <HardwareIcon className="w-[16px] h-[16px] text-brand" />
                          <span className="text-ink text-[13px] font-bold">{hardware.name}</span>
                          <span className="text-dim text-[10px] uppercase">{hardware.type.replace('_', ' ')}</span>
                          {!hasProducers && (
                            <span className="bg-yellow-600/20 border border-yellow-600/50 text-yellow-500 px-[5px] py-[1px] text-[9px] uppercase font-bold leading-none">
                              0 Producers
                            </span>
                          )}
                        </div>
                        <div className="flex items-center gap-[4px]">
                          <button
                            onClick={() => openEditHardwareModal(hardware)}
                            disabled={mutationsLocked}
                            aria-label={`Edit ${hardware.id}`}
                            title={mutationsLocked ? lockedTitle : 'Edit'}
                            className="p-[5px] bg-surface hover:bg-brand text-dim hover:text-white disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-surface disabled:hover:text-dim transition-colors rounded"
                          >
                            <Edit className="w-[13px] h-[13px]" />
                          </button>
                          <button
                            onClick={() => handleDeleteHardware(hardware.id)}
                            disabled={mutationsLocked || undeletableHardware}
                            aria-label={`Delete ${hardware.id}`}
                            title={mutationsLocked ? lockedTitle : undeletableHardware ? viewOnlyTitle : 'Delete'}
                            className="p-[5px] bg-surface hover:bg-red-600 text-dim hover:text-white disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-surface disabled:hover:text-dim transition-colors rounded"
                          >
                            <Trash2 className="w-[13px] h-[13px]" />
                          </button>
                          {hasProducers ? (
                            <button
                              onClick={() => toggleHardwareExpand(hardware.id)}
                              className="flex items-center gap-[4px] px-[8px] py-[4px] bg-surface hover:bg-dim text-dim hover:text-app transition-colors text-[10px] uppercase rounded"
                            >
                              {isExpanded ? <ChevronUp className="w-[12px] h-[12px]" /> : <ChevronDown className="w-[12px] h-[12px]" />}
                              Producers ({hardware.producers.length})
                            </button>
                          ) : (
                            <button
                              onClick={() => openAddProducerModal(hardware.id, hardware)}
                              disabled={mutationsLocked}
                              title={mutationsLocked ? lockedTitle : ''}
                              className="flex items-center gap-[4px] px-[8px] py-[4px] bg-surface hover:bg-dim text-dim hover:text-app disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-surface disabled:hover:text-dim transition-colors text-[10px] uppercase rounded"
                            >
                              <Plus className="w-[12px] h-[12px]" />
                              Add Producer
                            </button>
                          )}
                        </div>
                      </div>

                      {/* Hardware-specific fields */}
                      <div className="mt-[8px]">
                        {hardware.type.includes('camera') && renderCameraFields(hardware as CameraHardware)}
                        {hardware.type === 'trossen_arm' && renderArmFields(hardware as ArmHardware)}
                        {/* A handle IS a leader arm, so its Glide controls
                            belong on its own card rather than in a panel that
                            names it from a distance. */}
                        {hardware.type === 'trossen_arm' &&
                          isGlideHandle(hardware as ArmHardware) &&
                          renderGlideHandle(hardware as ArmHardware)}
                        {(hardware.type === 'slate_base' || hardware.type === 'trossen_base') && renderBaseFields(hardware as BaseHardware)}
                      </div>
                    </div>

                    {/* Level 3: Producers (nested inside hardware) */}
                    {isExpanded && (
                      <div className="border-t border-surface bg-app p-[16px]">
                        <div className="flex items-center justify-between mb-[12px]">
                          <div className="flex items-center gap-[8px]">
                            <Radio className="w-[14px] h-[14px] text-dim" />
                            <h3 className="text-[12px] text-ink uppercase">Producers</h3>
                            <span className="text-dim text-[10px]">({hardware.producers.length})</span>
                          </div>
                          <button
                            onClick={() => openAddProducerModal(hardware.id, hardware)}
                            disabled={mutationsLocked}
                            title={mutationsLocked ? lockedTitle : ''}
                            className="bg-brand text-white px-[10px] py-[5px] text-[10px] uppercase hover:bg-[#4aa8cc] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-brand transition-colors"
                          >
                            <Plus className="w-[10px] h-[10px] inline mr-[4px]" />
                            Add Producer
                          </button>
                        </div>

                        {hardware.producers.length === 0 ? (
                          <div className="text-dim text-[11px] text-center py-[20px]">
                            No producers configured. Add one to create a data stream.
                          </div>
                        ) : (
                          <div className="space-y-[8px]">
                            {hardware.producers.map(producer => (
                              <div key={producer.id} className="bg-edge p-[12px] flex items-center justify-between">
                                <div className="flex-1 grid grid-cols-4 portrait:grid-cols-2 gap-[12px] text-[12px]">
                                  <div>
                                    <div className="text-dim text-[9px] uppercase mb-[4px]">Stream ID</div>
                                    <div className="text-ink font-mono">{producer.stream_id}</div>
                                  </div>
                                  <div>
                                    <div className="text-dim text-[9px] uppercase mb-[4px]">Poll Rate</div>
                                    <div className="text-ink">{producer.poll_rate_hz} Hz</div>
                                  </div>
                                  {producer.encoding && (
                                    <div>
                                      <div className="text-dim text-[9px] uppercase mb-[4px]">Encoding</div>
                                      <div className="text-ink">{producer.encoding}</div>
                                    </div>
                                  )}
                                  <div>
                                    <div className="text-dim text-[9px] uppercase mb-[4px]">Device Time</div>
                                    <div className="text-ink">{producer.use_device_time ? 'Yes' : 'No'}</div>
                                  </div>
                                </div>
                                <div className="flex items-center gap-[4px]">
                                  <button
                                    onClick={() => openEditProducerModal(hardware.id, producer)}
                                    disabled={mutationsLocked}
                                    aria-label={`Edit producer ${producer.id}`}
                                    title={mutationsLocked ? lockedTitle : 'Edit producer'}
                                    className="p-[6px] hover:bg-surface disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-transparent transition-colors"
                                  >
                                    <Edit className="w-[14px] h-[14px] text-brand" />
                                  </button>
                                  <button
                                    onClick={() => handleDeleteProducer(hardware.id, producer.id)}
                                    disabled={mutationsLocked}
                                    aria-label={`Delete producer ${producer.id}`}
                                    title={mutationsLocked ? lockedTitle : 'Delete producer'}
                                    className="p-[6px] hover:bg-surface disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-transparent transition-colors"
                                  >
                                    <Trash2 className="w-[14px] h-[14px] text-red-500" />
                                  </button>
                                </div>
                              </div>
                            ))}
                          </div>
                        )}
                      </div>
                    )}
                  </div>
                );
              })}

              {selectedSystemData.hardware.length === 0 && (
                <div className="text-center py-[40px] text-dim">
                  <Server className="w-[48px] h-[48px] mx-auto mb-[16px] opacity-50" />
                  <p className="text-[14px] mb-[8px]">No hardware configured</p>
                  <p className="text-[12px]">Add cameras, arms, or bases to this system</p>
                </div>
              )}
            </div>
          </div>

          {/* --- Teleoperation ------------------------------------------------
              Below the hardware, because it wires hardware together and reads
              as nonsense before you know what arms exist.

              Modelled here rather than passed through: the SDK only WARNS about
              a pair naming a missing arm and then skips it, so a rig can look
              configured and move nothing. */}
          {(() => {
            const sys = selectedSystemData;
            const arms = sys.hardware.filter((h) => h.type === 'trossen_arm') as ArmHardware[];
            const leaders = arms.filter((a) => a.role === 'leader');
            const followers = arms.filter((a) => a.role === 'follower');
            const teleop = sys.teleop ?? { enabled: false, rate_hz: DEFAULT_TELEOP_RATE_HZ, pairs: [] };

            // Shown when the rig can actually teleoperate — one of each. The
            // extra clauses keep an existing teleop block visible on a rig that
            // no longer qualifies; hiding a panel that still holds live config
            // is how config gets edited blind and then silently rewritten.
            const canTeleop = leaders.length > 0 && followers.length > 0;
            if (!canTeleop && teleop.pairs.length === 0 && !teleop.enabled) return null;

            const armNames = new Set(arms.map((a) => a.name));
            const orphaned = teleop.pairs.filter(
              (pr) => !armNames.has(pr.leader) || !armNames.has(pr.follower),
            );
            const paired = new Set(teleop.pairs.flatMap((pr) => [pr.leader, pr.follower]));
            const unpaired = [...leaders, ...followers].filter((a) => !paired.has(a.name));

            const patch = (next: Partial<TeleopModel>) =>
              setSystems((prev) =>
                prev.map((x) =>
                  x.id === sys.id ? { ...x, teleop: { ...teleop, ...next } } : x,
                ),
              );
            const setPair = (i: number, next: Partial<TeleopPair>) =>
              patch({ pairs: teleop.pairs.map((pr, j) => (j === i ? { ...pr, ...next } : pr)) });

            return (
              <div className="mt-[16px] border border-edge">
                <div className="flex items-center justify-between px-[12px] py-[8px] border-b border-edge">
                  <div className="text-brand text-[10px] uppercase font-bold">Teleoperation</div>
                  <label className="flex items-center gap-[6px] text-[11px] text-dim cursor-pointer">
                    <input
                      type="checkbox"
                      checked={teleop.enabled}
                      onChange={(e) => patch({ enabled: e.target.checked })}
                    />
                    Enabled
                  </label>
                </div>
                <div className="p-[12px] space-y-[10px]">
                  {!canTeleop && (
                    <div className="text-[11px] text-red-400">
                      This system has {leaders.length} leader{leaders.length !== 1 ? 's' : ''} and{' '}
                      {followers.length} follower{followers.length !== 1 ? 's' : ''}. Teleop needs at
                      least one of each; what is configured here will be dropped on save.
                    </div>
                  )}
                  {canTeleop && teleop.pairs.length === 0 && (
                    <div className="text-[11px] text-yellow-500">
                      Leader and follower arms are configured but none are linked, so nothing will
                      move. Add a pair below.
                    </div>
                  )}
                  {canTeleop && teleop.pairs.length > 0 && !teleop.enabled && (
                    <div className="text-[11px] text-yellow-500">
                      Teleoperation is switched off, so these pairs are ignored and the followers
                      will not track their leaders.
                    </div>
                  )}
                  {unpaired.length > 0 && teleop.pairs.length > 0 && (
                    <div className="text-[11px] text-yellow-500">
                      Not in any pair: {unpaired.map((a) => a.name).join(', ')} — {unpaired.length === 1 ? 'it' : 'they'} will
                      hold position.
                    </div>
                  )}
                  {orphaned.length > 0 && (
                    <div className="text-[11px] text-red-400">
                      {orphaned.length} pair{orphaned.length !== 1 ? 's' : ''} name an arm that
                      no longer exists and will be dropped on save.
                    </div>
                  )}
                  {teleop.pairs.map((pr, i) => (
                    <div key={i} className="grid grid-cols-[1fr_1fr_110px_32px] gap-[8px] items-center">
                      <select
                        value={pr.leader}
                        onChange={(e) => setPair(i, { leader: e.target.value })}
                        className="bg-app border border-edge text-ink px-[8px] py-[6px] text-[12px]"
                      >
                        <option value="">— leader —</option>
                        {leaders.map((a) => <option key={a.id} value={a.name}>{a.name}</option>)}
                        {pr.leader && !armNames.has(pr.leader) && (
                          <option value={pr.leader}>{pr.leader} (missing)</option>
                        )}
                      </select>
                      <select
                        value={pr.follower}
                        onChange={(e) => setPair(i, { follower: e.target.value })}
                        className="bg-app border border-edge text-ink px-[8px] py-[6px] text-[12px]"
                      >
                        <option value="">— follower —</option>
                        {followers.map((a) => <option key={a.id} value={a.name}>{a.name}</option>)}
                        {pr.follower && !armNames.has(pr.follower) && (
                          <option value={pr.follower}>{pr.follower} (missing)</option>
                        )}
                      </select>
                      <select
                        value={pr.space}
                        onChange={(e) => setPair(i, { space: e.target.value as TeleopSpace })}
                        className="bg-app border border-edge text-ink px-[8px] py-[6px] text-[12px]"
                      >
                        {TELEOP_SPACES.map((sp) => <option key={sp} value={sp}>{sp}</option>)}
                      </select>
                      <button
                        onClick={() => patch({ pairs: teleop.pairs.filter((_, j) => j !== i) })}
                        className="text-red-400 hover:text-red-300 text-[16px] leading-none"
                        title="Remove this pair"
                      >
                        &times;
                      </button>
                    </div>
                  ))}
                  <div className="flex items-center gap-[12px] pt-[4px]">
                    <button
                      onClick={() =>
                        patch({
                          pairs: [
                            ...teleop.pairs,
                            {
                              leader: leaders[teleop.pairs.length]?.name ?? leaders[0]?.name ?? '',
                              follower: followers[teleop.pairs.length]?.name ?? followers[0]?.name ?? '',
                              space: 'joint' as TeleopSpace,
                            },
                          ],
                        })
                      }
                      disabled={!canTeleop}
                      className="border border-edge text-ink px-[10px] py-[4px] text-[11px] uppercase hover:border-brand disabled:opacity-40 disabled:cursor-not-allowed"
                    >
                      + Pair
                    </button>
                    <label className="flex items-center gap-[6px] text-[11px] text-dim">
                      Rate
                      <input
                        type="number"
                        value={teleop.rate_hz}
                        onChange={(e) => patch({ rate_hz: Number(e.target.value) })}
                        className="w-[90px] bg-app border border-edge text-ink px-[8px] py-[4px] text-[12px]"
                      />
                      Hz
                    </label>
                  </div>
                </div>
              </div>
            );
          })()}

          {/* --- Glide session-control timing --------------------------------
              Everything else about a handle now lives on the handle's own card.
              These two are properties of the single glide_session_control
              component rather than of any one handle, so they have nowhere else
              to go. Hidden entirely until something is bound. */}
          {(() => {
            const sys = selectedSystemData;
            const sc = sys.sessionControl;
            if (!sc) return null;
            const patchSc = (next: Partial<SessionControlModel>) =>
              setSystems((prev) =>
                prev.map((x) => (x.id === sys.id ? { ...x, sessionControl: { ...sc, ...next } } : x)),
              );
            return (
              <div className="mt-[16px] border border-edge">
                <div className="px-[12px] py-[8px] border-b border-edge text-brand text-[10px] uppercase font-bold">
                  Handle Button Timing
                </div>
                <div className="p-[12px] flex flex-wrap items-center gap-[16px]">
                  <div className="text-dim text-[11px]">
                    {sc.buttons.length} binding{sc.buttons.length !== 1 ? 's' : ''} across all handles.
                    Shared by every button.
                  </div>
                  <label className="flex items-center gap-[6px] text-[11px] text-dim">
                    Poll
                    <input
                      type="number"
                      value={sc.poll_rate_hz ?? 50}
                      onChange={(e) => patchSc({ poll_rate_hz: Number(e.target.value) })}
                      className="w-[70px] bg-app border border-edge text-ink px-[8px] py-[4px] text-[12px]"
                    />
                    Hz
                  </label>
                  <label className="flex items-center gap-[6px] text-[11px] text-dim">
                    Debounce
                    <input
                      type="number"
                      value={sc.debounce_ms ?? 40}
                      onChange={(e) => patchSc({ debounce_ms: Number(e.target.value) })}
                      className="w-[70px] bg-app border border-edge text-ink px-[8px] py-[4px] text-[12px]"
                    />
                    ms
                  </label>
                </div>
              </div>
            );
          })()}
        </div>
      )}
      {/* End of !isLoading && !loadError guard */}
      </>)}

      {/* Add Producer Modal */}
      {showAddProducerModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-surface border border-edge w-full max-w-[500px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-edge">
              <h2 className="text-[18px] text-ink">{editingItem ? 'Edit Producer' : 'Add Producer'}</h2>
              <button onClick={() => setShowAddProducerModal(false)} className="text-[24px] text-dim hover:text-ink">×</button>
            </div>
            <form onSubmit={handleAddProducer} className="p-[20px] space-y-[16px]">
              {(() => {
                const hardware = currentParentHardwareId && selectedSystemData?.hardware.find(h => h.id === currentParentHardwareId);
                const mode = hardware ? getProducerMode(hardware.type) : 'poll';
                const isPoll = mode === 'poll';
                const isPush = mode === 'push';

                return (
                  <>
                    <div>
                      <label className="block text-ink text-[12px] mb-[8px]">Stream ID <span className="text-red-500">*</span></label>
                      <input
                        type="text"
                        value={producerForm.stream_id}
                        onChange={e => setProducerForm({ ...producerForm, stream_id: e.target.value })}
                        placeholder="e.g., camera_main_30fps"
                        className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                        required
                      />
                    </div>

                    {/* Poll Mode - Show poll_rate_hz slider */}
                    {isPoll && (
                      <div>
                        <label className="block text-ink text-[12px] mb-[8px]">Poll Rate (Hz) <span className="text-red-500">*</span></label>
                        <input
                          type="number"
                          min="1"
                          max="120"
                          value={producerForm.poll_rate_hz}
                          onChange={e => setProducerForm({ ...producerForm, poll_rate_hz: parseInt(e.target.value) })}
                          className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                          required
                        />
                        <div className="flex gap-[8px] mt-[8px]">
                          {[15, 30, 60].map(preset => (
                            <button
                              key={preset}
                              type="button"
                              onClick={() => setProducerForm({ ...producerForm, poll_rate_hz: preset })}
                              className={`px-[12px] py-[4px] text-[11px] transition-colors ${
                                producerForm.poll_rate_hz === preset
                                  ? 'bg-brand text-white'
                                  : 'bg-edge text-dim hover:bg-surface'
                              }`}
                            >
                              {preset} Hz
                            </button>
                          ))}
                        </div>
                      </div>
                    )}

                    {/* Push Mode - Show timeout_ms and info text */}
                    {isPush && (
                      <div>
                        <div className="bg-edge border border-brand/30 p-[12px] mb-[12px]">
                          <div className="text-brand text-[11px] font-bold uppercase mb-[4px]">Push Mode</div>
                          <div className="text-dim text-[11px]">Rate set by camera FPS on hardware card. Device delivers data on its own thread.</div>
                        </div>
                        <div>
                          <label className="block text-ink text-[12px] mb-[8px]">Timeout (ms)</label>
                          <input
                            type="number"
                            min="100"
                            max="10000"
                            value={producerForm.timeout_ms}
                            onChange={e => setProducerForm({ ...producerForm, timeout_ms: parseInt(e.target.value) })}
                            className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                          />
                          <div className="text-dim text-[10px] mt-[4px]">Default: 3000ms</div>
                        </div>
                      </div>
                    )}

                    {currentParentHardwareId && hardware && typeof hardware !== 'string' && hardware.type.includes('camera') && (
                      <div>
                        <label className="block text-ink text-[12px] mb-[8px]">Encoding</label>
                        <select
                          value={producerForm.encoding}
                          onChange={e => setProducerForm({ ...producerForm, encoding: e.target.value as 'bgr8' | 'rgb8' | 'mono8' })}
                          className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                        >
                          <option value="bgr8">bgr8</option>
                          <option value="rgb8">rgb8</option>
                          <option value="mono8">mono8</option>
                        </select>
                      </div>
                    )}

                    <div className="flex items-center gap-[8px]">
                      <input
                        type="checkbox"
                        id="use_device_time"
                        checked={producerForm.use_device_time}
                        onChange={e => setProducerForm({ ...producerForm, use_device_time: e.target.checked })}
                        className="w-[16px] h-[16px]"
                      />
                      <label htmlFor="use_device_time" className="text-ink text-[12px]">Use device time</label>
                    </div>
                  </>
                );
              })()}

              <div className="flex justify-end gap-[12px] pt-[12px]">
                <button type="button" onClick={() => setShowAddProducerModal(false)} className="bg-app border border-edge text-dim px-[20px] py-[10px] text-[14px] hover:border-white hover:text-ink transition-colors">Cancel</button>
                <button type="submit" className="bg-brand text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">{editingItem ? 'Save Producer' : 'Add Producer'}</button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Add Hardware Modal */}
      {showAddHardwareModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-surface border border-edge w-full max-w-[600px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-edge">
              <h2 className="text-[18px] text-ink">{editingHardwareId ? 'Edit' : 'Add'} {selectedHardwareType === 'camera' ? 'Camera' : selectedHardwareType === 'arm' ? 'Arm' : 'Base'}</h2>
              <button onClick={() => setShowAddHardwareModal(false)} className="text-[24px] text-dim hover:text-ink">×</button>
            </div>

            {selectedHardwareType === 'camera' && (
              <form onSubmit={handleAddCamera} className="p-[20px] space-y-[16px]">
                <div>
                  <label className="block text-ink text-[12px] mb-[8px]">Camera Type</label>
                  <select
                    value={selectedCameraType}
                    onChange={e => setSelectedCameraType(e.target.value as 'realsense_camera' | 'opencv_camera' | 'zed_camera')}
                    className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                  >
                    <option value="realsense_camera">RealSense</option>
                    <option value="opencv_camera">OpenCV</option>
                    <option value="zed_camera">ZED</option>
                  </select>
                </div>
                <div>
                  <label className="block text-ink text-[12px] mb-[8px]">Name <span className="text-red-500">*</span></label>
                  <input type="text" value={cameraForm.name} onChange={e => setCameraForm({ ...cameraForm, name: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                </div>
                {/* A ZED takes a named resolution and ignores width/height
                    entirely, so it gets a picker instead of two inputs that
                    would do nothing. Every other camera type is sized in
                    pixels. */}
                {selectedCameraType === 'zed_camera' ? (
                  <div className="grid grid-cols-2 gap-[12px]">
                    <div>
                      <label htmlFor="zed_resolution" className="block text-ink text-[12px] mb-[8px]">Resolution</label>
                      <select
                        id="zed_resolution"
                        value={cameraForm.resolution}
                        onChange={e => setCameraForm({ ...cameraForm, resolution: e.target.value as ZedResolution })}
                        className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                      >
                        {ZED_RESOLUTION_NAMES.map(name => {
                          const dims = ZED_RESOLUTIONS[name];
                          return (
                            <option key={name} value={name}>
                              {dims ? `${name} — ${dims.width}x${dims.height}` : `${name} — negotiated at open`}
                            </option>
                          );
                        })}
                      </select>
                    </div>
                    <div>
                      <label htmlFor="zed_fps" className="block text-ink text-[12px] mb-[8px]">FPS</label>
                      <input id="zed_fps" type="number" value={cameraForm.fps} onChange={e => setCameraForm({ ...cameraForm, fps: parseInt(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                    </div>
                  </div>
                ) : (
                  <div className="grid grid-cols-3 gap-[12px]">
                    <div>
                      <label className="block text-ink text-[12px] mb-[8px]">Width</label>
                      <input type="number" value={cameraForm.width} onChange={e => setCameraForm({ ...cameraForm, width: parseInt(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                    </div>
                    <div>
                      <label className="block text-ink text-[12px] mb-[8px]">Height</label>
                      <input type="number" value={cameraForm.height} onChange={e => setCameraForm({ ...cameraForm, height: parseInt(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                    </div>
                    <div>
                      <label className="block text-ink text-[12px] mb-[8px]">FPS</label>
                      <input type="number" value={cameraForm.fps} onChange={e => setCameraForm({ ...cameraForm, fps: parseInt(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                    </div>
                  </div>
                )}

                {selectedCameraType === 'realsense_camera' && (
                  <>
                    <div>
                      <label className="block text-ink text-[12px] mb-[8px]">Serial Number</label>
                      <input type="text" value={cameraForm.serial_number} onChange={e => setCameraForm({ ...cameraForm, serial_number: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                    </div>
                    <div className="flex items-center gap-[8px]">
                      <input type="checkbox" id="use_depth" checked={cameraForm.use_depth} onChange={e => setCameraForm({ ...cameraForm, use_depth: e.target.checked })} className="w-[16px] h-[16px]" />
                      <label htmlFor="use_depth" className="text-ink text-[12px]">Enable depth</label>
                    </div>
                  </>
                )}

                {selectedCameraType === 'opencv_camera' && (
                  <>
                    <div>
                      <label className="block text-ink text-[12px] mb-[8px]">Device Index</label>
                      <input type="text" value={cameraForm.device_index} onChange={e => setCameraForm({ ...cameraForm, device_index: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                    </div>
                    <div className="grid grid-cols-2 gap-[12px]">
                      <div>
                        <label className="block text-ink text-[12px] mb-[8px]">Backend</label>
                        <input type="text" value={cameraForm.backend} onChange={e => setCameraForm({ ...cameraForm, backend: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                      </div>
                      <div>
                        <label className="block text-ink text-[12px] mb-[8px]">Warmup Frames</label>
                        <input type="number" value={cameraForm.warmup_frames} onChange={e => setCameraForm({ ...cameraForm, warmup_frames: parseInt(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                      </div>
                    </div>
                  </>
                )}

                {selectedCameraType === 'zed_camera' && (
                  <>
                    <div>
                      <label htmlFor="zed_serial" className="block text-ink text-[12px] mb-[8px]">Serial Number</label>
                      <input id="zed_serial" type="text" inputMode="numeric" value={cameraForm.serial_number} onChange={e => setCameraForm({ ...cameraForm, serial_number: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                      <span className="block text-dim text-[10px] mt-[3px]">Numeric ZED serial. Required — the SDK opens the camera by serial, not by index.</span>
                    </div>
                    {/* Depth is gated entirely on this flag: a depth mode
                        without it is inert, which is why the mode selector only
                        appears once depth is on. */}
                    <div className="flex items-start gap-[8px]">
                      <input type="checkbox" id="zed_use_depth" checked={cameraForm.use_depth} onChange={e => setCameraForm({ ...cameraForm, use_depth: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                      <label htmlFor="zed_use_depth" className="text-ink text-[12px]">
                        Record depth
                        <span className="block text-dim text-[10px] mt-[3px]">Adds a 16-bit depth stream alongside colour. Costs GPU time and roughly doubles the write rate for this camera.</span>
                      </label>
                    </div>
                    {cameraForm.use_depth && (
                      <div>
                        <label htmlFor="zed_depth_mode" className="block text-ink text-[12px] mb-[8px]">Depth Mode</label>
                        <select
                          id="zed_depth_mode"
                          value={cameraForm.depth_mode}
                          onChange={e => setCameraForm({ ...cameraForm, depth_mode: e.target.value as ZedDepthMode })}
                          className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                        >
                          <option value="NEURAL_LIGHT">Neural Light — cheapest, start here</option>
                          <option value="NEURAL">Neural — balanced</option>
                          <option value="NEURAL_PLUS">Neural Plus — most accurate, most GPU</option>
                        </select>
                        <span className="block text-dim text-[10px] mt-[3px]">Every camera runs its own depth pass, so cost multiplies across a multi-ZED rig.</span>
                      </div>
                    )}
                  </>
                )}

                <div className="flex justify-end gap-[12px] pt-[12px]">
                  <button type="button" onClick={() => setShowAddHardwareModal(false)} className="bg-app border border-edge text-dim px-[20px] py-[10px] text-[14px] hover:border-white hover:text-ink transition-colors">Cancel</button>
                  <button type="submit" className="bg-brand text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">Add Camera</button>
                </div>
              </form>
            )}

            {selectedHardwareType === 'arm' && (
              <form onSubmit={handleAddArm} className="p-[20px] space-y-[16px]">
                <div>
                  <label className="block text-ink text-[12px] mb-[8px]">Name <span className="text-red-500">*</span></label>
                  <input type="text" value={armForm.name} onChange={e => setArmForm({ ...armForm, name: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                </div>
                <div>
                  <label className="block text-ink text-[12px] mb-[8px]">IP Address</label>
                  <input type="text" value={armForm.ip_address} onChange={e => setArmForm({ ...armForm, ip_address: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                </div>
                <div className="grid grid-cols-2 gap-[12px]">
                  <div>
                    <label className="block text-ink text-[12px] mb-[8px]">Model</label>
                    {/* Closed set: the driver rejects anything outside MODEL_NAME at
                        configure() time, so a free-text field could only ever produce
                        an arm that fails to boot. */}
                    <select value={armForm.model} onChange={e => setArmForm({ ...armForm, model: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand">
                      {ARM_MODELS.map(m => <option key={m} value={m}>{m}</option>)}
                      {!asArmModel(armForm.model) && (
                        // An existing config may name a model this build does not
                        // know. Show it rather than silently switching the arm to
                        // something else the moment the modal opens.
                        <option value={armForm.model}>{armForm.model} (not recognised)</option>
                      )}
                    </select>
                  </div>
                  <div>
                    <label className="block text-ink text-[12px] mb-[8px]">End Effector</label>
                    <select value={armForm.end_effector} onChange={e => setArmForm({ ...armForm, end_effector: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand">
                      {END_EFFECTORS.map(e2 => <option key={e2} value={e2}>{e2}</option>)}
                      {!asEndEffector(armForm.end_effector) && (
                        <option value={armForm.end_effector}>{armForm.end_effector} (not recognised)</option>
                      )}
                    </select>
                    {armForm.model === 'pro' && armForm.end_effector !== 'pro_base' && (
                      // Not an SDK error — it loads the wrong mass into gravity
                      // compensation and the arm just holds position badly.
                      <p className="text-[11px] text-yellow-500 mt-[6px]">
                        A Pro normally carries <span className="font-mono">pro_base</span>; another
                        end effector loads the wrong mass into gravity compensation.
                      </p>
                    )}
                  </div>
                </div>
                <div>
                  <label className="block text-ink text-[12px] mb-[8px]">Role</label>
                  <select value={armForm.role} onChange={e => setArmForm({ ...armForm, role: e.target.value as 'leader' | 'follower' })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand">
                    <option value="leader">Leader</option>
                    <option value="follower">Follower</option>
                  </select>
                </div>
                {armForm.role === 'leader' && (
                  <div className="space-y-[12px]">
                    <div className="flex items-start gap-[8px]">
                      <input type="checkbox" id="arm_passive" checked={armForm.passive} onChange={e => setArmForm({ ...armForm, passive: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                      <label htmlFor="arm_passive" className="text-ink text-[12px]">
                        Passive leader (lightweight — no actuators)
                        <span className="block text-dim text-[11px] mt-[2px]">Streams joint positions only; the SDK applies the lightweight-leader joint remap (J3/J4 inverted, wrist ±π/4).</span>
                      </label>
                    </div>
                    {armForm.passive && (
                      <div className="pl-[24px]">
                        <label className="block text-ink text-[12px] mb-[6px]">Wrist offset side</label>
                        <select value={armForm.wristSide} onChange={e => setArmForm({ ...armForm, wristSide: e.target.value as WristSide })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand">
                          <option value="left">Left (−π/4)</option>
                          <option value="right">Right (+π/4)</option>
                        </select>
                        <span className="block text-dim text-[11px] mt-[2px]">Sign of the J5 wrist-roll offset. Mirror-mounted left/right arms need opposite signs — flip this if the follower's wrist tracks rotated the wrong way.</span>
                      </div>
                    )}
                  </div>
                )}
                {armForm.role === 'leader' && (
                  <div className="space-y-[12px]">
                    <div className="flex items-start gap-[8px]">
                      <input type="checkbox" id="arm_gripper_feedback" checked={armForm.gripperFeedback} onChange={e => setArmForm({ ...armForm, gripperFeedback: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                      <label htmlFor="arm_gripper_feedback" className="text-ink text-[12px]">
                        Gripper force feedback
                        <span className="block text-dim text-[11px] mt-[2px]">Render the follower's grip force on this leader's gripper so the operator feels the grasp (cubic curve). Requires an actuated gripper — fine on a passive-arm lightweight leader. The follower gripper stays position passthrough.</span>
                      </label>
                    </div>
                    {armForm.gripperFeedback && (
                      <div className="pl-[24px] grid grid-cols-2 gap-[12px]">
                        <div>
                          <label className="block text-ink text-[12px] mb-[6px]">Leader max (N)</label>
                          <input type="number" step="any" value={armForm.gripperFeedbackLeaderMax} onChange={e => setArmForm({ ...armForm, gripperFeedbackLeaderMax: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                          <span className="block text-dim text-[11px] mt-[2px]">Effort rendered on the leader gripper at full grip (reference: 27).</span>
                        </div>
                        <div>
                          <label className="block text-ink text-[12px] mb-[6px]">Follower max (N)</label>
                          <input type="number" step="any" value={armForm.gripperFeedbackFollowerMax} onChange={e => setArmForm({ ...armForm, gripperFeedbackFollowerMax: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                          <span className="block text-dim text-[11px] mt-[2px]">Follower grip effort treated as full grip — normalizes the curve (reference: 87.5).</span>
                        </div>
                        <div>
                          <label className="block text-ink text-[12px] mb-[6px]">Offset (N)</label>
                          <input type="number" step="any" value={armForm.gripperFeedbackOffset} onChange={e => setArmForm({ ...armForm, gripperFeedbackOffset: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" />
                          <span className="block text-dim text-[11px] mt-[2px]">Baseline effort that keeps the leader gripper open when nothing is grasped (reference: 8).</span>
                        </div>
                      </div>
                    )}
                  </div>
                )}
                <div className="space-y-[12px]">
                  <div className="flex items-start gap-[8px]">
                    <input type="checkbox" id="arm_limits" checked={armForm.limitsEnabled} onChange={e => setArmForm({ ...armForm, limitsEnabled: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                    <label htmlFor="arm_limits" className="text-ink text-[12px]">
                      Set joint limits
                      <span className="block text-dim text-[11px] mt-[2px]">Per-joint velocity, position, and effort caps pushed to the controller. The control box does not keep these across a power cycle, so the SDK re-applies them on every connect. Leave off to use the controller's firmware defaults. The values below start permissive — tighten per joint as needed.</span>
                    </label>
                  </div>
                  {armForm.limitsEnabled && (
                    <div className="pl-[24px] overflow-x-auto">
                      <table className="text-[12px] border-collapse">
                        <thead>
                          <tr className="text-dim">
                            <th className="text-left font-normal py-[4px] pr-[8px]">Joint</th>
                            {LIMIT_COLUMNS.map(col => (
                              <th key={col.formKey} className="text-left font-normal py-[4px] px-[4px]">{col.label}</th>
                            ))}
                          </tr>
                        </thead>
                        <tbody>
                          {ARM_JOINT_LABELS.map((jointLabel, jointIdx) => {
                            const isGripper = jointIdx === NUM_ARM_JOINTS - 1;
                            return (
                              <tr key={jointLabel}>
                                <td className="text-ink py-[3px] pr-[8px] whitespace-nowrap">{jointLabel}</td>
                                {LIMIT_COLUMNS.map(col => (
                                  <td key={col.formKey} className="py-[3px] px-[4px]">
                                    <input
                                      type="number"
                                      step="any"
                                      value={armForm[col.formKey][jointIdx]}
                                      title={isGripper ? col.gripperUnit : col.armUnit}
                                      onChange={e => {
                                        const v = parseFloat(e.target.value);
                                        setArmForm(prev => {
                                          const next = [...prev[col.formKey]];
                                          next[jointIdx] = Number.isNaN(v) ? 0 : v;
                                          return { ...prev, [col.formKey]: next };
                                        });
                                      }}
                                      className="w-[72px] bg-app border border-edge text-ink px-[6px] py-[4px] text-[12px] focus:outline-none focus:border-brand"
                                    />
                                  </td>
                                ))}
                              </tr>
                            );
                          })}
                        </tbody>
                        <tfoot>
                          <tr className="text-dim">
                            <td className="pt-[6px] pr-[8px]" />
                            {LIMIT_COLUMNS.map(col => (
                              <td key={col.formKey} className="pt-[6px] px-[4px] text-[10px] whitespace-nowrap">{col.armUnit} · grip {col.gripperUnit}</td>
                            ))}
                          </tr>
                        </tfoot>
                      </table>
                    </div>
                  )}
                </div>
                <div className="space-y-[12px]">
                  <div className="flex items-start gap-[8px]">
                    <input type="checkbox" id="arm_tolerances" checked={armForm.tolerancesEnabled} onChange={e => setArmForm({ ...armForm, tolerancesEnabled: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                    <label htmlFor="arm_tolerances" className="text-ink text-[12px]">
                      Set joint tolerances
                      <span className="block text-dim text-[11px] mt-[2px]">Per-joint tolerances that PAD the limits above for the controller's feedback fault check — it errors if the measured position/velocity/effort exceeds the limit ± its tolerance. Also reset on power cycle, so re-applied every connect. Tick only the columns you mean to set; an unticked column keeps the controller's firmware default. Note that 0 is the <b className="text-ink">tightest</b> setting (fault on any overshoot), not a neutral one.</span>
                    </label>
                  </div>
                  {armForm.tolerancesEnabled && (
                    <div className="pl-[24px] overflow-x-auto">
                      <table className="text-[12px] border-collapse">
                        <thead>
                          <tr className="text-dim">
                            <th className="text-left font-normal py-[4px] pr-[8px]">Joint</th>
                            {TOLERANCE_COLUMNS.map(col => (
                              <th key={col.formKey} className="text-left font-normal py-[4px] px-[4px]">
                                <label className="flex items-center gap-[5px] cursor-pointer">
                                  <input
                                    type="checkbox"
                                    checked={armForm[col.setKey]}
                                    aria-label={`Set ${col.label}`}
                                    onChange={e => setArmForm(prev => ({ ...prev, [col.setKey]: e.target.checked }))}
                                    className="w-[12px] h-[12px]"
                                  />
                                  <span className={armForm[col.setKey] ? 'text-ink' : 'text-dim'}>{col.label}</span>
                                </label>
                              </th>
                            ))}
                          </tr>
                        </thead>
                        <tbody>
                          {ARM_JOINT_LABELS.map((jointLabel, jointIdx) => {
                            const isGripper = jointIdx === NUM_ARM_JOINTS - 1;
                            return (
                              <tr key={jointLabel}>
                                <td className="text-ink py-[3px] pr-[8px] whitespace-nowrap">{jointLabel}</td>
                                {TOLERANCE_COLUMNS.map(col => {
                                  const isSet = armForm[col.setKey];
                                  return (
                                    <td key={col.formKey} className="py-[3px] px-[4px]">
                                      <input
                                        type="number"
                                        step="any"
                                        // Blank rather than 0 when the column is
                                        // off, so "firmware default" never reads
                                        // as "zero tolerance".
                                        value={isSet ? armForm[col.formKey][jointIdx] : ''}
                                        placeholder="—"
                                        disabled={!isSet}
                                        title={isSet ? (isGripper ? col.gripperUnit : col.armUnit) : 'Firmware default — tick the column heading to set it'}
                                        onChange={e => {
                                          const v = parseFloat(e.target.value);
                                          setArmForm(prev => {
                                            const next = [...prev[col.formKey]];
                                            next[jointIdx] = Number.isNaN(v) ? 0 : v;
                                            return { ...prev, [col.formKey]: next };
                                          });
                                        }}
                                        className="w-[72px] bg-app border border-edge text-ink px-[6px] py-[4px] text-[12px] focus:outline-none focus:border-brand disabled:opacity-40 disabled:cursor-not-allowed"
                                      />
                                    </td>
                                  );
                                })}
                              </tr>
                            );
                          })}
                        </tbody>
                        <tfoot>
                          <tr className="text-dim">
                            <td className="pt-[6px] pr-[8px]" />
                            {TOLERANCE_COLUMNS.map(col => (
                              <td key={col.formKey} className="pt-[6px] px-[4px] text-[10px] whitespace-nowrap">{col.armUnit} · grip {col.gripperUnit}</td>
                            ))}
                          </tr>
                        </tfoot>
                      </table>
                    </div>
                  )}
                </div>
                <div className="space-y-[12px]">
                  {/* Command clamp. Bounds what teleop may ASK for, as opposed
                      to the joint limits above which bound what the arm will
                      accept — trimming here means no limit fault and no
                      interrupted session. */}
                  <div className={armForm.role === 'leader' ? 'opacity-40 pointer-events-none' : ''}>
                    <div className="flex items-start gap-[8px]">
                      <input
                        type="checkbox"
                        id="arm_command_clamp"
                        disabled={armForm.role === 'leader'}
                        checked={armForm.commandClampEnabled}
                        onChange={(e) => setArmForm({ ...armForm, commandClampEnabled: e.target.checked })}
                        className="w-[16px] h-[16px] mt-[2px]"
                      />
                      <label htmlFor="arm_command_clamp" className="text-ink text-[12px]">
                        Clamp incoming teleop commands
                        <span className="block text-dim text-[11px] mt-[2px]">
                          Bounds the pose this arm will be asked for, per joint. A leader can
                          reach places its follower should not go — another arm, the base, a
                          shelf — and clamping here trims the command before it is sent, so the
                          joint parks at the bound instead of the controller raising a limit
                          fault and ending the session. Leave a cell blank to leave that joint
                          alone.
                        </span>
                      </label>
                    </div>
                    {armForm.commandClampEnabled && (
                      <div className="pl-[24px] mt-[10px] overflow-x-auto">
                        <table className="text-[11px] border-separate border-spacing-0">
                          <thead>
                            <tr className="text-dim">
                              <th className="text-left font-normal pr-[8px] pb-[4px]">Joint</th>
                              <th className="text-left font-normal px-[4px] pb-[4px]">Min</th>
                              <th className="text-left font-normal px-[4px] pb-[4px]">Max</th>
                            </tr>
                          </thead>
                          <tbody>
                            {ARM_JOINT_LABELS.map((jointLabel, jointIdx) => {
                              const isGripper = jointIdx === NUM_ARM_JOINTS - 1;
                              const cols = [
                                { key: 'commandClampMin' as const, label: 'min' },
                                { key: 'commandClampMax' as const, label: 'max' },
                              ];
                              return (
                                <tr key={jointLabel}>
                                  <td className="text-ink py-[3px] pr-[8px] whitespace-nowrap">{jointLabel}</td>
                                  {cols.map((col) => (
                                    <td key={col.key} className="py-[3px] px-[4px]">
                                      <input
                                        type="number"
                                        step="any"
                                        value={armForm[col.key][jointIdx] ?? ''}
                                        placeholder="—"
                                        title={`${isGripper ? 'm' : 'rad'} — blank leaves this joint unclamped`}
                                        onChange={(e) => {
                                          const raw = e.target.value;
                                          const v = raw === '' ? null : parseFloat(raw);
                                          setArmForm((prev) => {
                                            const next = [...prev[col.key]];
                                            next[jointIdx] = v !== null && Number.isNaN(v) ? null : v;
                                            return { ...prev, [col.key]: next };
                                          });
                                        }}
                                        className="w-[86px] bg-app border border-edge text-ink px-[6px] py-[4px] text-[12px] focus:outline-none focus:border-brand"
                                      />
                                    </td>
                                  ))}
                                </tr>
                              );
                            })}
                          </tbody>
                          <tfoot>
                            <tr className="text-dim">
                              <td className="pt-[6px] pr-[8px]" />
                              <td className="pt-[6px] px-[4px] text-[10px]" colSpan={2}>
                                rad · gripper m
                              </td>
                            </tr>
                          </tfoot>
                        </table>
                        {(() => {
                          const bad = ARM_JOINT_LABELS.map((label, i) => {
                            const lo = armForm.commandClampMin[i];
                            const hi = armForm.commandClampMax[i];
                            return lo !== null && hi !== null && lo > hi ? label : null;
                          }).filter(Boolean);
                          if (bad.length === 0) return null;
                          return (
                            <div className="text-[11px] text-red-400 mt-[6px]">
                              {bad.join(', ')}: min is above max, which pins the joint. The SDK
                              refuses to configure an arm like this.
                            </div>
                          );
                        })()}
                      </div>
                    )}
                  </div>
                  {/* Goal time. Inert on a leader for the same reason smoothing
                      is, so it is disabled there rather than hidden — an
                      operator looking for it should find it greyed with the
                      reason, not wonder where it went. */}
                  <div className={armForm.role === 'leader' ? 'opacity-40 pointer-events-none' : ''}>
                    <label htmlFor="write_moving_time" className="block text-dim text-[11px] mb-[4px]">Command goal time (s)</label>
                    <input
                      id="write_moving_time"
                      type="number"
                      step="any"
                      min="0"
                      disabled={armForm.role === 'leader'}
                      value={armForm.writeMovingTimeS}
                      onChange={e => {
                        const v = parseFloat(e.target.value);
                        setArmForm({ ...armForm, writeMovingTimeS: Number.isNaN(v) || v < 0 ? 0 : v });
                      }}
                      className="w-[160px] bg-app border border-edge text-ink px-[10px] py-[8px] text-[12px] focus:outline-none focus:border-brand disabled:cursor-not-allowed"
                    />
                    <span className="block text-dim text-[11px] mt-[3px]">
                      How long the controller is given to reach each command.{' '}
                      <span className="text-ink">0 = apply immediately</span>, which is what a
                      real-time mirror wants and what the SDK defaults to.
                    </span>
                  </div>
                  {armForm.role !== 'leader' && armForm.writeMovingTimeS > 0 && (
                    <div className="text-[11px] text-yellow-500 border border-yellow-500/40 p-[8px]">
                      A non-zero goal time is a lag, not a smoother. Teleop issues a new
                      command every tick, so at {DEFAULT_TELEOP_RATE_HZ} Hz this one is
                      replaced after ~{(1000 / DEFAULT_TELEOP_RATE_HZ).toFixed(1)} ms —{' '}
                      {((100 / (armForm.writeMovingTimeS * DEFAULT_TELEOP_RATE_HZ)) || 0).toFixed(1)}% of
                      the way through its {armForm.writeMovingTimeS}s trajectory. The follower
                      never arrives; it trails the leader by roughly that time constant. To
                      damp jitter without losing tracking, use command smoothing below.
                    </div>
                  )}
                  {/* One-Euro filters the COMMANDED pose on its way into the
                      controller (`cmd_filt_.filter(pos_d, ...)`), so it only does
                      anything on an arm that receives commands. A passive leader
                      gets none, which made these fields look meaningful on a card
                      where they could never take effect. */}
                  {armForm.role === 'leader' && (
                    <div className="text-[11px] text-dim border border-edge/60 p-[8px]">
                      Command smoothing applies to the arm that <span className="text-ink">receives</span>{' '}
                      commands. Set it on the follower this leader drives — a leader is
                      commanded by the operator's hand, not by the SDK.
                    </div>
                  )}
                  <div className={`flex items-start gap-[8px] ${armForm.role === 'leader' ? 'opacity-40 pointer-events-none' : ''}`}>
                    <input type="checkbox" id="arm_smoothing" disabled={armForm.role === 'leader'} checked={armForm.smoothingEnabled} onChange={e => setArmForm({ ...armForm, smoothingEnabled: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                    <label htmlFor="arm_smoothing" className="text-ink text-[12px]">
                      Smooth outgoing commands
                      <span className="block text-dim text-[11px] mt-[2px]">One-Euro adaptive low-pass on the position commands sent to this arm. Unlike a fixed low-pass it raises its cutoff as the command speeds up, so slow motion is smoothed hard while fast motion keeps its response. Use it on a follower whose leader input is jittery. Off leaves commands untouched.</span>
                    </label>
                  </div>
                  {armForm.smoothingEnabled && (
                    <div className="pl-[24px] space-y-[12px]">
                      <div className="grid grid-cols-3 portrait:grid-cols-1 gap-[12px]">
                        <div>
                          <label htmlFor="smoothing_min_cutoff" className="block text-dim text-[11px] mb-[4px]">Min cutoff (Hz)</label>
                          <input
                            id="smoothing_min_cutoff"
                            type="number"
                            step="any"
                            min="0"
                            value={armForm.smoothingMinCutoffHz}
                            onChange={e => setArmForm({ ...armForm, smoothingMinCutoffHz: parseFloat(e.target.value) })}
                            className="w-full bg-app border border-edge text-ink px-[10px] py-[8px] text-[12px] focus:outline-none focus:border-brand"
                          />
                          <span className="block text-dim text-[10px] mt-[3px]">Cutoff at rest. Lower = smoother, more lag.</span>
                        </div>
                        <div>
                          <label htmlFor="smoothing_beta" className="block text-dim text-[11px] mb-[4px]">Beta</label>
                          <input
                            id="smoothing_beta"
                            type="number"
                            step="any"
                            min="0"
                            value={armForm.smoothingBeta}
                            onChange={e => setArmForm({ ...armForm, smoothingBeta: parseFloat(e.target.value) })}
                            className="w-full bg-app border border-edge text-ink px-[10px] py-[8px] text-[12px] focus:outline-none focus:border-brand"
                          />
                          <span className="block text-dim text-[10px] mt-[3px]">Speed coefficient. Higher = less lag when moving fast.</span>
                        </div>
                        <div>
                          <label htmlFor="smoothing_d_cutoff" className="block text-dim text-[11px] mb-[4px]">Derivative cutoff (Hz)</label>
                          <input
                            id="smoothing_d_cutoff"
                            type="number"
                            step="any"
                            min="0"
                            value={armForm.smoothingDCutoffHz}
                            onChange={e => setArmForm({ ...armForm, smoothingDCutoffHz: parseFloat(e.target.value) })}
                            className="w-full bg-app border border-edge text-ink px-[10px] py-[8px] text-[12px] focus:outline-none focus:border-brand"
                          />
                          <span className="block text-dim text-[10px] mt-[3px]">Smoothing on the speed estimate itself.</span>
                        </div>
                      </div>
                      <div className="flex items-start gap-[8px]">
                        <input type="checkbox" id="arm_smoothing_gripper" checked={armForm.smoothingGripper} onChange={e => setArmForm({ ...armForm, smoothingGripper: e.target.checked })} className="w-[16px] h-[16px] mt-[2px]" />
                        <label htmlFor="arm_smoothing_gripper" className="text-ink text-[12px]">
                          Smooth the gripper too
                          <span className="block text-dim text-[11px] mt-[2px]">Off by default: the gripper is filtered separately from the arm joints because lag on a grasp is felt directly by the operator.</span>
                        </label>
                      </div>
                    </div>
                  )}
                </div>
                <div className="flex justify-end gap-[12px] pt-[12px]">
                  <button type="button" onClick={() => setShowAddHardwareModal(false)} className="bg-app border border-edge text-dim px-[20px] py-[10px] text-[14px] hover:border-white hover:text-ink transition-colors">Cancel</button>
                  <button type="submit" className="bg-brand text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">Add Arm</button>
                </div>
              </form>
            )}

            {selectedHardwareType === 'base' && (
              <form onSubmit={handleAddBase} className="p-[20px] space-y-[16px]">
                {editingBaseType === 'trossen_base' ? (
                  <>
                    {/* Identity is fixed: the Rivet base is declared as a
                        hardware.components entry and this page patches that
                        entry by id, so renaming here would relabel the card
                        without changing anything the SDK reads. */}
                    <div>
                      <div className="block text-dim text-[10px] uppercase mb-[4px]">Component</div>
                      <div className="text-ink text-[14px]">{baseForm.name} <span className="text-dim text-[12px]">· trossen_base</span></div>
                    </div>

                    {/* ── Drive ────────────────────────────────────────── */}
                    <div className="border-t border-edge pt-[14px]">
                      <div className="text-dim text-[10px] uppercase mb-[10px] tracking-wide">Drive</div>
                      <div className="grid grid-cols-2 gap-[12px]">
                        <div>
                          <label htmlFor="base_max_linear" className="block text-ink text-[12px] mb-[8px]">Max Linear</label>
                          <input id="base_max_linear" type="number" step="any" min="0" value={baseForm.max_linear_mps} onChange={e => setBaseForm({ ...baseForm, max_linear_mps: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                          <span className="block text-dim text-[10px] mt-[3px]">m/s. The base clamps every command to this.</span>
                        </div>
                        <div>
                          <label htmlFor="base_max_angular" className="block text-ink text-[12px] mb-[8px]">Max Angular</label>
                          <input id="base_max_angular" type="number" step="any" min="0" value={baseForm.max_angular_rps} onChange={e => setBaseForm({ ...baseForm, max_angular_rps: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                          <span className="block text-dim text-[10px] mt-[3px]">rad/s.</span>
                        </div>
                      </div>
                    </div>

                    {/* ── Linear rail ──────────────────────────────────── */}
                    <div className="border-t border-edge pt-[14px]">
                      <div className="text-dim text-[10px] uppercase mb-[10px] tracking-wide">Linear Rail</div>
                      <div>
                        <label htmlFor="base_max_lift" className="block text-ink text-[12px] mb-[8px]">Max Speed</label>
                        <input id="base_max_lift" type="number" step="any" min="0" value={baseForm.max_lift_units_per_s} onChange={e => setBaseForm({ ...baseForm, max_lift_units_per_s: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                        <span className="block text-dim text-[10px] mt-[3px]">
                          Driver units/s, not m/s — the base exposes the vertical axis that way.
                        </span>
                      </div>
                      <div className="mt-[10px] flex items-start gap-[6px] border border-brand/40 bg-brand/[0.06] px-[10px] py-[8px]">
                        <Radio className="w-[12px] h-[12px] text-brand shrink-0 mt-[3px]" />
                        <span className="text-dim text-[11px] leading-[1.5]">
                          This value is applied twice on save: as the base's own ceiling and as
                          the leader handle's lift-axis limit. They act in series, so setting
                          only one would leave the rail capped by the other.
                        </span>
                      </div>
                    </div>

                    {/* ── Safety ───────────────────────────────────────── */}
                    <div className="border-t border-edge pt-[14px]">
                      <div className="text-dim text-[10px] uppercase mb-[10px] tracking-wide">Safety</div>
                      <div className="grid grid-cols-2 gap-[12px]">
                        <div>
                          <label htmlFor="base_estop_battery" className="block text-ink text-[12px] mb-[8px]">E-Stop Battery</label>
                          <input id="base_estop_battery" type="number" step="any" min="0" max="100" value={baseForm.estop_battery_percent} onChange={e => setBaseForm({ ...baseForm, estop_battery_percent: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                          <span className="block text-dim text-[10px] mt-[3px]">
                            Percent. <b className="text-ink">0 disables the check.</b> Above zero, the
                            recorder e-stops the base and homes the arms after ~1.5&nbsp;s below this level.
                          </span>
                        </div>
                        <div>
                          <label htmlFor="base_ready_timeout" className="block text-ink text-[12px] mb-[8px]">Ready Timeout</label>
                          <input id="base_ready_timeout" type="number" step="any" min="0" value={baseForm.ready_timeout_s} onChange={e => setBaseForm({ ...baseForm, ready_timeout_s: parseFloat(e.target.value) })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                          <span className="block text-dim text-[10px] mt-[3px]">Seconds to wait for the base to report ready before failing startup.</span>
                        </div>
                      </div>
                    </div>
                  </>
                ) : (
                  <>
                    <div>
                      <label htmlFor="base_name" className="block text-ink text-[12px] mb-[8px]">Name <span className="text-red-500">*</span></label>
                      <input id="base_name" type="text" value={baseForm.name} onChange={e => setBaseForm({ ...baseForm, name: e.target.value })} className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand" required />
                    </div>
                    <div className="flex items-center gap-[8px]">
                      <input type="checkbox" id="reset_odometry" checked={baseForm.reset_odometry} onChange={e => setBaseForm({ ...baseForm, reset_odometry: e.target.checked })} className="w-[16px] h-[16px]" />
                      <label htmlFor="reset_odometry" className="text-ink text-[12px]">Reset odometry</label>
                    </div>
                    <div className="flex items-center gap-[8px]">
                      <input type="checkbox" id="enable_torque" checked={baseForm.enable_torque} onChange={e => setBaseForm({ ...baseForm, enable_torque: e.target.checked })} className="w-[16px] h-[16px]" />
                      <label htmlFor="enable_torque" className="text-ink text-[12px]">Enable torque</label>
                    </div>
                  </>
                )}
                <div className="flex justify-end gap-[12px] pt-[12px]">
                  <button type="button" onClick={() => setShowAddHardwareModal(false)} className="bg-app border border-edge text-dim px-[20px] py-[10px] text-[14px] hover:border-white hover:text-ink transition-colors">Cancel</button>
                  <button type="submit" className="bg-brand text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">{editingHardwareId ? 'Save Base' : 'Add Base'}</button>
                </div>
              </form>
            )}
          </div>
        </div>
      )}

      {/* Add System Modal */}
      {showAddSystemModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-surface border border-edge w-full max-w-[500px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-edge">
              <h2 className="text-[18px] text-ink">{editingSystemId ? 'Edit' : 'Create'} Hardware System</h2>
              <button onClick={() => setShowAddSystemModal(false)} className="text-[24px] text-dim hover:text-ink">×</button>
            </div>
            <form onSubmit={handleAddSystem} className="p-[20px] space-y-[16px]">
              <div>
                <label className="block text-ink text-[12px] mb-[8px]">System Name <span className="text-red-500">*</span></label>
                <input
                  type="text"
                  value={systemForm.name}
                  onChange={e => setSystemForm({ ...systemForm, name: e.target.value })}
                  placeholder="e.g., Custom System, VR Rig, UMI Setup"
                  className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand"
                  required
                />
              </div>
              <div>
                <label className="block text-ink text-[12px] mb-[8px]">Description</label>
                <textarea
                  value={systemForm.description}
                  onChange={e => setSystemForm({ ...systemForm, description: e.target.value })}
                  placeholder="Brief description of this hardware system"
                  rows={3}
                  className="w-full bg-app border border-edge text-ink px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-brand resize-none"
                />
              </div>
              <div className="flex justify-end gap-[12px] pt-[12px]">
                <button type="button" onClick={() => setShowAddSystemModal(false)} className="bg-app border border-edge text-dim px-[20px] py-[10px] text-[14px] hover:border-white hover:text-ink transition-colors">Cancel</button>
                <button type="submit" className="bg-brand text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">{editingSystemId ? 'Save Changes' : 'Create System'}</button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Hardware Type Selection Modal */}
      {showHardwareTypeModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-surface border border-edge w-full max-w-[400px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-edge">
              <h2 className="text-[18px] text-ink">Select Hardware Type</h2>
              <button onClick={() => setShowHardwareTypeModal(false)} className="text-[24px] text-dim hover:text-ink">×</button>
            </div>
            <div className="p-[20px] space-y-[12px]">
              <button
                onClick={() => {
                  setShowHardwareTypeModal(false);
                  openAddHardwareModal('camera');
                }}
                className="w-full bg-edge border border-edge hover:border-brand p-[16px] flex items-center gap-[12px] transition-colors group"
              >
                <Camera className="w-[20px] h-[20px] text-brand" />
                <div className="text-left">
                  <div className="text-ink text-[14px] font-bold">Camera</div>
                  <div className="text-dim text-[11px]">RealSense, OpenCV, ZED</div>
                </div>
              </button>
              <button
                onClick={() => {
                  setShowHardwareTypeModal(false);
                  openAddHardwareModal('arm');
                }}
                className="w-full bg-edge border border-edge hover:border-brand p-[16px] flex items-center gap-[12px] transition-colors group"
              >
                <Bot className="w-[20px] h-[20px] text-brand" />
                <div className="text-left">
                  <div className="text-ink text-[14px] font-bold">Arm</div>
                  <div className="text-dim text-[11px]">Trossen robotics arms</div>
                </div>
              </button>
              <button
                onClick={() => {
                  setShowHardwareTypeModal(false);
                  openAddHardwareModal('base');
                }}
                className="w-full bg-edge border border-edge hover:border-brand p-[16px] flex items-center gap-[12px] transition-colors group"
              >
                <Smartphone className="w-[20px] h-[20px] text-brand" />
                <div className="text-left">
                  <div className="text-ink text-[14px] font-bold">Mobile Base</div>
                  <div className="text-dim text-[11px]">SLATE mobile base</div>
                </div>
              </button>
            </div>
          </div>
        </div>
      )}

      {/* App-level modal (replaces native alert / confirm) */}
      {appModal && (
        <AppModal
          title={appModal.title}
          message={appModal.message}
          variant={appModal.variant}
          confirmLabel={appModal.confirmLabel}
          onConfirm={appModal.onConfirm}
          onCancel={appModal.onCancel}
        />
      )}
    </div>
  );
}
