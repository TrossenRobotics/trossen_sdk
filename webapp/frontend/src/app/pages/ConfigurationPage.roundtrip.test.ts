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
import { sdkConfigToSystem, systemToSdkConfig } from './ConfigurationPage';

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
        { id: 'camera_main', type: 'zed_camera', serial_number: '51287468', width: 1920, height: 1200, fps: 30 },
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

describe('the whole config', () => {
  it('round-trips a Rivet without losing a section', () => {
    const original = rivetConfig();
    const { saved } = roundTrip(original);
    expect(saved.teleop).toEqual(original.teleop);
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
