/**
 * Round-trip tests for the Configuration page's config <-> UI conversion.
 *
 * The page does not edit the SDK config in place; it parses the config into a
 * flat UI model and rebuilds the config from that model on save. Anything the
 * parse step drops is therefore DELETED from a saved config, silently, and the
 * page keeps rendering as though it were still there.
 *
 * That failure mode shipped three times at once on the Rivet: the camera type,
 * the whole of `hardware.components` (including the base), and the arms'
 * command-smoothing block. These tests pin the round-trip instead of the
 * individual fields, so a fourth instance fails here rather than on a robot.
 */
import { describe, it, expect } from 'vitest';
import {
  sdkConfigToSystem,
  setGlideButtonBinding,
  setGlideHandleInput,
  systemToSdkConfig,
} from './ConfigurationPage';

/** A cut-down Rivet: the decomposed shape, with one of everything that matters. */
function rivetConfig() {
  return {
    robot_name: 'rivet',
    hardware: {
      arms: {
        glide_left: {
          ip_address: '192.168.1.4',
          model: 'glide_left',
          end_effector: 'none',
          actuated: false,
          smoothing_enabled: true,
          smoothing_min_cutoff_hz: 2.5,
          smoothing_beta: 0.4,
          smoothing_d_cutoff_hz: 1.5,
        },
        follower_left: {
          ip_address: '192.168.1.2',
          model: 'wxai_v0',
          end_effector: 'wxai_v0_follower',
        },
      },
      cameras: [
        {
          id: 'camera_main',
          type: 'zed_camera',
          serial_number: '51287468',
          resolution: 'HD1200',
          width: 1920,
          height: 1200,
          fps: 30,
          use_depth: false,
        },
      ],
      components: [
        { id: 'glide_inputs', type: 'glide_arm_input', arms: ['glide_left'] },
        {
          id: 'rivet_base',
          type: 'trossen_base',
          max_linear_mps: 0.6,
          max_angular_rps: 1.2,
          max_lift_units_per_s: 8000.0,
          estop_battery_percent: 15.0,
        },
        {
          id: 'base_leader',
          type: 'glide_base',
          translation: { arm_id: 'glide_left', forward_source: 'joystick_y', max: 0.6 },
          // The rail's other ceiling. Applied in series with the base's
          // max_lift_units_per_s, so the two have to move together.
          axes: {
            angular: { arm_id: 'glide_left', source: 'joystick_x', max: 1.2 },
            lift: { arm_id: 'glide_left', source: 'buttons', up_bit: 0, down_bit: 2, max: 8000.0 },
          },
        },
      ],
    },
    producers: [
      { type: 'trossen_arm', hardware_id: 'glide_left', stream_id: 'glide_left', poll_rate_hz: 30.0, use_device_time: false },
      { type: 'trossen_arm', hardware_id: 'follower_left', stream_id: 'follower_left', poll_rate_hz: 30.0, use_device_time: false },
      { type: 'trossen_base', hardware_id: 'rivet_base', stream_id: 'rivet_base', poll_rate_hz: 30.0, use_device_time: false },
      { type: 'zed_camera', hardware_id: 'camera_main', stream_id: 'camera_main', poll_rate_hz: 30.0, encoding: 'bgr8', use_device_time: true },
    ],
    teleop: { mode: 'glide' },
    backend: { kind: 'mcap' },
    session: { episode_duration_s: 30 },
  };
}

/** Parse then immediately re-serialise, i.e. "open the page and press Save". */
function roundTrip(config: ReturnType<typeof rivetConfig>) {
  const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config });
  return { system, saved: systemToSdkConfig(system, config) };
}

describe('camera type', () => {
  it('survives a save', () => {
    // The regression that broke ZED on the Orin. CameraConfig::type defaults to
    // "realsense_camera" in the SDK, so dropping this key does not fail loudly
    // — it silently reassigns the hardware.
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.cameras?.[0].type).toBe('zed_camera');
  });

  it('is read from the hardware entry, not the producer', () => {
    // The SDK builds the camera from hardware.cameras[].type. If the two ever
    // disagree, the hardware entry is the one that decides.
    const config = rivetConfig();
    config.producers[3].type = 'realsense_camera';
    const { system } = roundTrip(config);
    const camera = system.hardware.find((h) => h.id === 'camera_main');
    expect(camera?.type).toBe('zed_camera');
  });

  it('falls back to the producer type for configs that only carry it there', () => {
    const config = rivetConfig();
    delete (config.hardware.cameras[0] as { type?: string }).type;
    const { saved } = roundTrip(config);
    expect(saved.hardware?.cameras?.[0].type).toBe('zed_camera');
  });
});

describe('ZED camera keys', () => {
  it('keeps the resolution across a save', () => {
    // `resolution` is the ONLY thing that sets a ZED's frame size — the
    // component reads this named mode and ignores width/height entirely. Losing
    // it does not fail; it silently reverts an HD1200 rig to the component's
    // HD720 default, which is a quiet dataset regression rather than an error.
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.cameras?.[0].resolution).toBe('HD1200');
  });

  it('keeps depth enabled across a save', () => {
    // Depth is gated entirely on use_depth; depth_mode alone does nothing. When
    // this key was dropped on save, depth could not be turned on from the
    // webapp at all.
    const config = rivetConfig();
    config.hardware.cameras[0].use_depth = true;
    const { saved } = roundTrip(config);
    expect(saved.hardware?.cameras?.[0].use_depth).toBe(true);
  });

  it('keeps depth disabled across a save', () => {
    // The false case matters just as much: silently dropping it would leave the
    // SDK defaulting to false and look identical, hiding the bug.
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.cameras?.[0].use_depth).toBe(false);
  });

  it('derives width and height from the resolution, not from the config', () => {
    // The ZED negotiates its frame size from `resolution`, so width/height in
    // the config are advisory at best and stale at worst. The UI must show what
    // the camera will actually produce.
    const config = rivetConfig();
    config.hardware.cameras[0].width = 4;
    config.hardware.cameras[0].height = 4;
    const { system } = roundTrip(config);
    const camera = system.hardware.find((h) => h.id === 'camera_main');
    expect(camera).toMatchObject({ width: 1920, height: 1200 });
  });

  it('upgrades depth modes the SDK no longer recognises', () => {
    // An older build of this page wrote lowercase 'performance' / 'quality' /
    // 'ultra'. The component compares depth-mode strings exactly, so those fell
    // through to its "unknown depth_mode" fallback with only a stderr warning —
    // invisible from the webapp. Heal them on load instead.
    const config = rivetConfig();
    (config.hardware.cameras[0] as { depth_mode?: string }).depth_mode = 'ultra';
    const { saved } = roundTrip(config);
    expect(saved.hardware?.cameras?.[0].depth_mode).toBe('NEURAL_PLUS');
  });

  it('supplies a resolution for a ZED config that never had one', () => {
    // Making the SDK's own HD720 fallback explicit, rather than leaving the
    // frame size to an unstated default.
    const config = rivetConfig();
    delete (config.hardware.cameras[0] as { resolution?: string }).resolution;
    const { saved } = roundTrip(config);
    expect(saved.hardware?.cameras?.[0].resolution).toBe('HD720');
  });
});

describe('the linear rail', () => {
  it('reports the leader axis limit alongside the base ceiling', () => {
    const { system } = roundTrip(rivetConfig());
    const base = system.hardware.find((h) => h.id === 'rivet_base');
    expect(base).toMatchObject({ max_lift_units_per_s: 8000, lift_leader_max: 8000, lift_leader_id: 'base_leader' });
  });

  it('writes a raised ceiling to BOTH the base and the leader axis', () => {
    // The whole point of surfacing the rail as one control. The leader scales
    // the lift command and the base clamps it, so raising only one leaves the
    // rail capped by the other — the operator sees a changed number and no
    // change in behaviour.
    const original = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: original });
    const raised = {
      ...system,
      hardware: system.hardware.map((h) =>
        h.id === 'rivet_base' ? { ...h, max_lift_units_per_s: 12000 } : h,
      ),
    };

    const saved = systemToSdkConfig(raised, original);
    const base = saved.hardware?.components?.find((c) => c.id === 'rivet_base');
    const leader = saved.hardware?.components?.find((c) => c.id === 'base_leader');

    expect(base?.max_lift_units_per_s).toBe(12000);
    expect((leader?.axes as { lift: { max: number } }).lift.max).toBe(12000);
  });

  it('leaves the rest of the leader axis untouched when syncing the ceiling', () => {
    // The glide_base component is otherwise opaque to this page: its button
    // bits and its other axes must survive the lift patch byte-for-byte.
    const original = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: original });
    const raised = {
      ...system,
      hardware: system.hardware.map((h) =>
        h.id === 'rivet_base' ? { ...h, max_lift_units_per_s: 12000 } : h,
      ),
    };

    const leader = systemToSdkConfig(raised, original).hardware?.components?.find(
      (c) => c.id === 'base_leader',
    );
    expect(leader?.translation).toEqual(original.hardware.components[2].translation);
    expect((leader?.axes as { angular: unknown }).angular).toEqual({
      arm_id: 'glide_left',
      source: 'joystick_x',
      max: 1.2,
    });
    expect((leader?.axes as { lift: Record<string, unknown> }).lift).toMatchObject({
      arm_id: 'glide_left',
      source: 'buttons',
      up_bit: 0,
      down_bit: 2,
    });
  });

  it('does not invent a lift axis on a leader that has none', () => {
    const original = rivetConfig();
    delete (original.hardware.components[2] as { axes?: unknown }).axes;
    const { saved, system } = roundTrip(original);

    const base = system.hardware.find((h) => h.id === 'rivet_base');
    expect(base).toMatchObject({ lift_leader_max: undefined, lift_leader_id: undefined });
    expect(saved.hardware?.components?.[2]).not.toHaveProperty('axes');
  });
});

describe('the Rivet base', () => {
  it('round-trips every ceiling it edits', () => {
    const original = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: original });
    const edited = {
      ...system,
      hardware: system.hardware.map((h) =>
        h.id === 'rivet_base'
          ? {
              ...h,
              max_linear_mps: 1.0,
              max_angular_rps: 1.8,
              estop_battery_percent: 22,
              ready_timeout_s: 45,
            }
          : h,
      ),
    };

    const base = systemToSdkConfig(edited, original).hardware?.components?.find(
      (c) => c.id === 'rivet_base',
    );
    expect(base).toMatchObject({
      type: 'trossen_base',
      max_linear_mps: 1.0,
      max_angular_rps: 1.8,
      estop_battery_percent: 22,
      ready_timeout_s: 45,
    });
  });

  it('keeps a zero battery threshold rather than treating it as unset', () => {
    // Zero is the component's own default and means "disabled" — a meaningful
    // value, not a missing one. Dropping it as falsy would re-enable the
    // previous threshold on the next save.
    const original = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: original });
    const disabled = {
      ...system,
      hardware: system.hardware.map((h) =>
        h.id === 'rivet_base' ? { ...h, estop_battery_percent: 0 } : h,
      ),
    };

    const base = systemToSdkConfig(disabled, original).hardware?.components?.find(
      (c) => c.id === 'rivet_base',
    );
    expect(base?.estop_battery_percent).toBe(0);
  });
});

describe('hardware.components', () => {
  it('survives a save in full', () => {
    // Rebuilding this array from the UI model would delete every component the
    // page has no card for — on a Rivet that is the base, the Glide input
    // handles, the base leader and session control.
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.components).toEqual(rivetConfig().hardware.components);
  });

  it('preserves keys of unmodelled components byte-for-byte', () => {
    const { saved } = roundTrip(rivetConfig());
    const baseLeader = saved.hardware?.components?.find((c) => c.id === 'base_leader');
    expect(baseLeader?.translation).toEqual({ arm_id: 'glide_left', forward_source: 'joystick_y', max: 0.6 });
  });

  it('surfaces the base, including the lift, as editable hardware', () => {
    const { system } = roundTrip(rivetConfig());
    const base = system.hardware.find((h) => h.id === 'rivet_base');
    expect(base?.type).toBe('trossen_base');
    expect(base).toMatchObject({
      max_linear_mps: 0.6,
      max_angular_rps: 1.2,
      max_lift_units_per_s: 8000.0,
      estop_battery_percent: 15.0,
    });
  });

  it('keeps the base out of the cameras array', () => {
    // A trossen_base that misses its branch in the writer lands in the `else`,
    // which is the camera branch.
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.cameras?.map((c) => c.id)).toEqual(['camera_main']);
  });

  it('keeps the base producer typed as trossen_base', () => {
    const { saved } = roundTrip(rivetConfig());
    const producer = saved.producers?.find((p) => p.hardware_id === 'rivet_base');
    expect(producer?.type).toBe('trossen_base');
  });
});

describe('arm command smoothing', () => {
  it('survives a save with its tuning', () => {
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.arms?.glide_left).toMatchObject({
      smoothing_enabled: true,
      smoothing_min_cutoff_hz: 2.5,
      smoothing_beta: 0.4,
      smoothing_d_cutoff_hz: 1.5,
    });
  });

  it('is not invented for arms that never asked for it', () => {
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.arms?.follower_left).not.toHaveProperty('smoothing_enabled');
  });
});

describe('arm command goal time', () => {
  // The field that made every Glide follower trail its leader by 0.3s. It was
  // set in the factory presets, invisible in the UI, and stacked on top of the
  // one-euro filter — so the page has to both preserve it and be able to clear
  // it.
  it('survives a save', () => {
    const cfg = rivetConfig();
    (cfg.hardware.arms.follower_left as Record<string, unknown>).write_moving_time_s = 0.3;
    const { saved } = roundTrip(cfg);
    expect(saved.hardware?.arms?.follower_left).toMatchObject({ write_moving_time_s: 0.3 });
  });

  it('is not invented for arms that never asked for it', () => {
    const { saved } = roundTrip(rivetConfig());
    expect(saved.hardware?.arms?.follower_left).not.toHaveProperty('write_moving_time_s');
  });

  it('drops the key when cleared to zero, which is the same thing to the SDK', () => {
    // `write_moving_time_s_` defaults to 0.0f, so absent and 0 are identical
    // to the driver. Dropping it keeps a real-time arm clean rather than
    // pinning a redundant zero into every config we touch.
    const cfg = rivetConfig();
    (cfg.hardware.arms.follower_left as Record<string, unknown>).write_moving_time_s = 0.3;
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
    const follower = system.hardware.find((h) => h.id === 'follower_left') as {
      write_moving_time_s?: number;
    };
    follower.write_moving_time_s = 0;
    const saved = systemToSdkConfig(system as never, cfg);
    expect(saved.hardware?.arms?.follower_left).not.toHaveProperty('write_moving_time_s');
  });
});

describe('the whole config', () => {
  it('round-trips a Rivet without losing a section', () => {
    const original = rivetConfig();
    const { saved } = roundTrip(original);
    // teleop is no longer passed through byte-for-byte — the page owns it now,
    // so it comes back normalised. Behaviour is unchanged: this fixture has no
    // pairs, and an absent `enabled` means true in the SDK, so both the old
    // shape and the new one yield zero teleop controllers. Keys the page does
    // not model still survive.
    expect(saved.teleop).toMatchObject({ mode: 'glide', enabled: false, pairs: [] });
    expect(saved.backend).toEqual(original.backend);
    expect(saved.session).toEqual(original.session);
    expect(saved.robot_name).toBe('rivet');
    // Every hardware id that went in comes back out.
    expect(Object.keys(saved.hardware?.arms ?? {}).sort()).toEqual(['follower_left', 'glide_left']);
    expect(saved.producers?.map((p) => p.hardware_id).sort()).toEqual(
      ['camera_main', 'follower_left', 'glide_left', 'rivet_base'],
    );
  });

  it('still drops a producer whose hardware the user deleted', () => {
    // The components passthrough must not become a back door that resurrects
    // producers for hardware genuinely removed in the UI.
    const config = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config });
    system.hardware = system.hardware.filter((h) => h.id !== 'camera_main');
    const saved = systemToSdkConfig(system, config);
    expect(saved.producers?.map((p) => p.hardware_id)).not.toContain('camera_main');
    expect(saved.hardware?.cameras).toEqual([]);
  });
});

describe('teleop pairs', () => {
  it('round-trips the pairs and the rate', () => {
    const cfg = rivetConfig();
    (cfg.teleop as Record<string, unknown>).enabled = true;
    (cfg.teleop as Record<string, unknown>).rate_hz = 1000;
    (cfg.teleop as Record<string, unknown>).pairs = [
      { leader: 'glide_left', follower: 'follower_left', space: 'joint' },
    ];
    const { saved } = roundTrip(cfg);
    expect(saved.teleop).toMatchObject({
      enabled: true,
      rate_hz: 1000,
      pairs: [{ leader: 'glide_left', follower: 'follower_left', space: 'joint' }],
    });
  });

  it('defaults a pair with no space to joint, as the SDK does', () => {
    const cfg = rivetConfig();
    (cfg.teleop as Record<string, unknown>).pairs = [
      { leader: 'glide_left', follower: 'follower_left' },
    ];
    const { saved } = roundTrip(cfg);
    expect(saved.teleop).toMatchObject({
      pairs: [{ leader: 'glide_left', follower: 'follower_left', space: 'joint' }],
    });
  });

  it('prunes a pair whose leader no longer exists', () => {
    // The failure that cost two sessions: arms deleted in the UI, pairs left
    // behind naming them. The SDK only logs "[warn] Leader ... not registered"
    // and carries on, so it is invisible until nothing moves.
    const cfg = rivetConfig();
    (cfg.teleop as Record<string, unknown>).pairs = [
      { leader: 'glide_left', follower: 'follower_left', space: 'joint' },
    ];
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
    const noLeaders = {
      ...system,
      hardware: system.hardware.filter((h) => !h.name.startsWith('glide_')),
    };
    const saved = systemToSdkConfig(noLeaders, cfg);
    expect((saved.teleop as { pairs: unknown[] }).pairs).toEqual([]);
    // ...and an enabled block with no pairs is meaningless, so it goes off.
    expect((saved.teleop as { enabled: boolean }).enabled).toBe(false);
  });

  it('keeps teleop off for a camera-only system rather than inventing pairs', () => {
    const cfg = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
    const camerasOnly = {
      ...system,
      hardware: system.hardware.filter((h) => h.type.includes('camera')),
    };
    const saved = systemToSdkConfig(camerasOnly, cfg);
    expect(saved.teleop).toMatchObject({ enabled: false, pairs: [] });
  });
});

describe('the Glide components', () => {
  it('round-trips glide_arm_input', () => {
    const { saved } = roundTrip(rivetConfig());
    const gi = saved.hardware?.components?.find((c) => c.type === 'glide_arm_input');
    expect(gi).toMatchObject({ id: 'glide_inputs', arms: ['glide_left'] });
  });

  it('round-trips session-control buttons', () => {
    const cfg = rivetConfig();
    cfg.hardware.components.push({
      id: 'session_control',
      type: 'glide_session_control',
      poll_rate_hz: 50.0,
      debounce_ms: 40,
      buttons: [
        { arm_id: 'glide_left', bit: 0, event: 'start' },
        { arm_id: 'glide_left', bit: 2, event: 'rerecord' },
      ],
    } as unknown as (typeof cfg.hardware.components)[number]);
    const { saved } = roundTrip(cfg);
    const sc = saved.hardware?.components?.find((c) => c.type === 'glide_session_control');
    expect(sc).toMatchObject({
      id: 'session_control',
      poll_rate_hz: 50,
      debounce_ms: 40,
      buttons: [
        { arm_id: 'glide_left', bit: 0, event: 'start' },
        { arm_id: 'glide_left', bit: 2, event: 'rerecord' },
      ],
    });
  });

  it('drops glide_arm_input when the user removes it', () => {
    const cfg = rivetConfig();
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
    const saved = systemToSdkConfig({ ...system, glideInputs: undefined }, cfg);
    expect(saved.hardware?.components?.some((c) => c.type === 'glide_arm_input')).toBe(false);
  });

  it('still preserves components it does not model', () => {
    // The whole reason the page used to pass components through untouched.
    const { saved } = roundTrip(rivetConfig());
    const types = (saved.hardware?.components ?? []).map((c) => c.type);
    expect(types).toContain('trossen_base');
    expect(types).toContain('glide_base');
  });

  it('appends a Glide component the original config never had', () => {
    const cfg = rivetConfig();
    cfg.hardware.components = cfg.hardware.components.filter(
      (c) => c.type !== 'glide_arm_input',
    );
    const system = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
    const saved = systemToSdkConfig(
      { ...system, glideInputs: { id: 'glide_inputs', arms: ['glide_left'] } },
      cfg,
    );
    expect(saved.hardware?.components?.find((c) => c.type === 'glide_arm_input')).toMatchObject({
      id: 'glide_inputs',
      arms: ['glide_left'],
    });
  });
});

describe('editing a Glide handle from its own card', () => {
  // Both components hard-fail in configure() when they are declared with
  // nothing in them, so the editor has to delete the component rather than
  // leave an empty one. That rule is what these cover.
  function rivetSystem() {
    const cfg = rivetConfig();
    cfg.hardware.components.push({
      id: 'session_control',
      type: 'glide_session_control',
      poll_rate_hz: 50.0,
      debounce_ms: 40,
      buttons: [{ arm_id: 'glide_left', bit: 0, event: 'start' }],
    } as unknown as (typeof cfg.hardware.components)[number]);
    return sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
  }

  describe('handle input', () => {
    it('adds a handle without disturbing the others', () => {
      const sys = { ...rivetSystem(), glideInputs: { id: 'glide_inputs', arms: ['glide_left'] } };
      expect(setGlideHandleInput(sys, 'glide_right', true).glideInputs).toEqual({
        id: 'glide_inputs',
        arms: ['glide_left', 'glide_right'],
      });
    });

    it('never lists the same handle twice', () => {
      const sys = { ...rivetSystem(), glideInputs: { id: 'glide_inputs', arms: ['glide_left'] } };
      expect(setGlideHandleInput(sys, 'glide_left', true).glideInputs?.arms).toEqual(['glide_left']);
    });

    it('drops the component when the last handle is switched off', () => {
      // Not an empty arms array: `GlideArmInputComponent::configure` throws
      // "requires a non-empty 'arms' array" on one.
      const sys = { ...rivetSystem(), glideInputs: { id: 'glide_inputs', arms: ['glide_left'] } };
      expect(setGlideHandleInput(sys, 'glide_left', false).glideInputs).toBeUndefined();
    });

    it('creates the component for the first handle', () => {
      const sys = { ...rivetSystem(), glideInputs: undefined };
      expect(setGlideHandleInput(sys, 'glide_left', true).glideInputs).toEqual({
        id: 'glide_inputs',
        arms: ['glide_left'],
      });
    });
  });

  describe('button bindings', () => {
    it('binds a free button', () => {
      const sc = setGlideButtonBinding(rivetSystem(), 'glide_left', 3, 'stop_session')
        .sessionControl;
      expect(sc?.buttons).toEqual([
        { arm_id: 'glide_left', bit: 0, event: 'start' },
        { arm_id: 'glide_left', bit: 3, event: 'stop_session' },
      ]);
    });

    it('replaces rather than duplicates a rebound button', () => {
      const sc = setGlideButtonBinding(rivetSystem(), 'glide_left', 0, 'rerecord').sessionControl;
      expect(sc?.buttons).toEqual([{ arm_id: 'glide_left', bit: 0, event: 'rerecord' }]);
    });

    it('keeps the same bit on a different handle separate', () => {
      // bit 0 is "top" on every handle, so the arm_id is the only thing telling
      // two of them apart.
      const sc = setGlideButtonBinding(rivetSystem(), 'glide_right', 0, 'start').sessionControl;
      expect(sc?.buttons).toHaveLength(2);
    });

    it('preserves poll rate and debounce', () => {
      const sc = setGlideButtonBinding(rivetSystem(), 'glide_left', 1, 'stop_early').sessionControl;
      expect(sc).toMatchObject({ id: 'session_control', poll_rate_hz: 50, debounce_ms: 40 });
    });

    it('drops the component when the last binding is cleared', () => {
      expect(
        setGlideButtonBinding(rivetSystem(), 'glide_left', 0, null).sessionControl,
      ).toBeUndefined();
    });

    it('creates the component, with SDK defaults, for the first binding', () => {
      const sys = { ...rivetSystem(), sessionControl: undefined };
      expect(setGlideButtonBinding(sys, 'glide_left', 0, 'start').sessionControl).toEqual({
        id: 'session_control',
        poll_rate_hz: 50,
        debounce_ms: 40,
        buttons: [{ arm_id: 'glide_left', bit: 0, event: 'start' }],
      });
    });

    it('survives the round trip back into the config', () => {
      // The editor's patch has to land somewhere the save path can see it.
      const cfg = rivetConfig();
      const sys = sdkConfigToSystem('rivet', { id: 'rivet', name: 'Rivet', config: cfg });
      const patched = { ...sys, ...setGlideButtonBinding(sys, 'glide_left', 3, 'stop_session') };
      const saved = systemToSdkConfig(patched, cfg);
      expect(
        saved.hardware?.components?.find((c) => c.type === 'glide_session_control'),
      ).toMatchObject({ buttons: [{ arm_id: 'glide_left', bit: 3, event: 'stop_session' }] });
    });
  });
});
