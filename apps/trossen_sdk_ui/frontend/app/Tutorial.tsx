import { driver } from 'driver.js';
import 'driver.js/dist/driver.css';

export const startWidowXTutorial = () => {
  const driverObj = driver({
    showProgress: true,
    allowClose: true,
    overlayClickNext: false,
    allowKeyboardControl: false,
    steps: [
      {
        element: '#tutorial-button',
        popover: {
          title: 'Welcome to SOMA',
          description: 'This tutorial will guide you through setting up a WidowX teleop leader-follower recording session. Let\'s get started!',
          side: 'bottom',
          align: 'start'
        }
      },
      {
        popover: {
          title: 'Tutorial Overview',
          description: 'We will walk through these steps:<ol><li>Configure two WidowX robots (leader and follower)</li><li>Set up a camera</li><li>Create producers to read robot data</li><li>Create a hardware system</li><li>Start recording your teleop session</li></ol>Let\'s begin by going to the Configuration page.',
        }
      },
      {
        element: 'nav',
        popover: {
          title: 'Navigation',
          description: 'Use these tabs to navigate between Dashboard, Configuration, and Record pages. Click "Configuration" to continue.',
          side: 'bottom',
        },
        onNextClick: () => {
          window.location.href = '/configuration';
          driverObj.moveNext();
        }
      },
      {
        element: '#robots-tab',
        popover: {
          title: 'Step 1: Add Robot Arms',
          description: 'First, let\'s configure two WidowX robots - a leader and a follower. Click the "Robots" tab to begin.',
          side: 'bottom',
        },
        onHighlighted: () => {
          const robotsTab = document.querySelector('#robots-tab') as HTMLElement;
          if (robotsTab) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              robotsTab.removeEventListener('click', handleClick);
            };
            robotsTab.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#add-robot-button',
        popover: {
          title: 'Add Leader Robot',
          description: 'Click "+ Add Robot" to open the configuration form.',
          side: 'left',
        },
        onHighlighted: () => {
          const addButton = document.querySelector('#add-robot-button') as HTMLElement;
          if (addButton) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              addButton.removeEventListener('click', handleClick);
            };
            addButton.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#robot-modal-form',
        popover: {
          title: 'Configure Leader Robot',
          description: 'Fill out the form:<ol><li>Select "WidowX" as robot type</li><li>Name it "leader"</li><li>Select "wxai_v0_leader" as end effector</li><li>Enter your robot\'s IP address</li><li>Click "Add Robot"</li></ol>The form will close and you can add the follower robot next.',
          side: 'right',
        },
        onHighlighted: () => {
          // Watch for modal to close (form submitted)
          const checkModalClosed = setInterval(() => {
            const modal = document.querySelector('#robot-modal-form');
            if (!modal) {
              clearInterval(checkModalClosed);
              setTimeout(() => driverObj.moveNext(), 300);
            }
          }, 100);

          // Clean up interval when moving to next step
          const cleanup = () => {
            clearInterval(checkModalClosed);
          };
          (driverObj as any)._cleanupRobotModal = cleanup;
        },
        onDeselected: () => {
          if ((driverObj as any)._cleanupRobotModal) {
            (driverObj as any)._cleanupRobotModal();
          }
        }
      },
      {
        element: '#robots-section',
        popover: {
          title: 'Add Follower Robot & Connect',
          description: 'Now:<ol><li>Click "+ Add Robot" again</li><li>Select "WidowX" as robot type</li><li>Name it "follower"</li><li>Select "wxai_v0_follower" as end effector</li><li>Enter the follower\'s IP address</li><li>Click "Add Robot"</li><li>Click "Connect" on BOTH robot cards</li></ol>Click "Next" when both robots are connected.',
          side: 'top',
        }
      },
      {
        element: '#producers-tab',
        popover: {
          title: 'Step 2: Create Teleop Producer',
          description: 'Now let\'s create a teleop producer that links the leader and follower robots together. Click the "Producers" tab.',
          side: 'bottom',
        },
        onHighlighted: () => {
          const producersTab = document.querySelector('#producers-tab') as HTMLElement;
          if (producersTab) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              producersTab.removeEventListener('click', handleClick);
            };
            producersTab.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#add-producer-button',
        popover: {
          title: 'Add Teleop Producer',
          description: 'Click "+ Add Producer" to open the producer form.',
          side: 'left',
        },
        onHighlighted: () => {
          const addButton = document.querySelector('#add-producer-button') as HTMLElement;
          if (addButton) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              addButton.removeEventListener('click', handleClick);
            };
            addButton.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#producer-modal-form',
        popover: {
          title: 'Configure Teleop Producer',
          description: 'Fill out the form:<ol><li>Select "Teleop WidowX Arm" as producer type</li><li>Select "leader" as the Leader Robot</li><li>Select "follower" as the Follower Robot</li><li>Click "Add Producer"</li></ol>This creates a producer that reads data from both arms during teleoperation.',
          side: 'right',
        },
        onHighlighted: () => {
          const checkModalClosed = setInterval(() => {
            const modal = document.querySelector('#producer-modal-form');
            if (!modal) {
              clearInterval(checkModalClosed);
              setTimeout(() => driverObj.moveNext(), 300);
            }
          }, 100);

          const cleanup = () => {
            clearInterval(checkModalClosed);
          };
          (driverObj as any)._cleanupProducerModal = cleanup;
        },
        onDeselected: () => {
          if ((driverObj as any)._cleanupProducerModal) {
            (driverObj as any)._cleanupProducerModal();
          }
        }
      },
      {
        element: '#systems-tab',
        popover: {
          title: 'Step 3: Create Hardware System',
          description: 'Now let\'s group the producer into a hardware system. Click the "Hardware Systems" tab.',
          side: 'bottom',
        },
        onHighlighted: () => {
          const systemsTab = document.querySelector('#systems-tab') as HTMLElement;
          if (systemsTab) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              systemsTab.removeEventListener('click', handleClick);
            };
            systemsTab.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#add-system-button',
        popover: {
          title: 'Add Hardware System',
          description: 'Click "+ Create System" to open the system form.',
          side: 'left',
        },
        onHighlighted: () => {
          const addButton = document.querySelector('#add-system-button') as HTMLElement;
          if (addButton) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              addButton.removeEventListener('click', handleClick);
            };
            addButton.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#system-modal-form',
        popover: {
          title: 'Configure Hardware System',
          description: 'Fill out the form:<ol><li>Name it "widowx_teleop"</li><li>Select your teleop producer from the list</li><li>Click "Add System"</li></ol>This creates a complete recording configuration.',
          side: 'right',
        },
        onHighlighted: () => {
          const checkModalClosed = setInterval(() => {
            const modal = document.querySelector('#system-modal-form');
            if (!modal) {
              clearInterval(checkModalClosed);
              setTimeout(() => driverObj.moveNext(), 300);
            }
          }, 100);

          const cleanup = () => {
            clearInterval(checkModalClosed);
          };
          (driverObj as any)._cleanupSystemModal = cleanup;
        },
        onDeselected: () => {
          if ((driverObj as any)._cleanupSystemModal) {
            (driverObj as any)._cleanupSystemModal();
          }
        }
      },
      {
        element: 'nav',
        popover: {
          title: 'Step 4: Create Recording Session',
          description: 'Perfect! Your hardware is configured. Now let\'s create a recording session. Click "Next" to go to the Record page.',
          side: 'bottom',
        },
        onNextClick: () => {
          window.location.href = '/record';
          driverObj.moveNext();
        }
      },
      {
        element: '#new-session-button',
        popover: {
          title: 'Create Your Session',
          description: 'Click "+ New Session" to open the session form.',
          side: 'left',
        },
        onHighlighted: () => {
          const addButton = document.querySelector('#new-session-button') as HTMLElement;
          if (addButton) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              addButton.removeEventListener('click', handleClick);
            };
            addButton.addEventListener('click', handleClick);
          }
        }
      },
      {
        element: '#session-modal-form',
        popover: {
          title: 'Configure Recording Session',
          description: 'Fill out the form:<ol><li>Name: "my_first_recording"</li><li>System: Select "widowx_teleop"</li><li>Action: "Teleop WidowX"</li><li>Episodes: 5</li><li>Duration: 30 seconds</li><li>Backend: "mcap" or "lerobot"</li></ol>Click "Create Session" when ready.',
          side: 'right',
        },
        onHighlighted: () => {
          const checkModalClosed = setInterval(() => {
            const modal = document.querySelector('#session-modal-form');
            if (!modal) {
              clearInterval(checkModalClosed);
              setTimeout(() => driverObj.moveNext(), 300);
            }
          }, 100);

          const cleanup = () => {
            clearInterval(checkModalClosed);
          };
          (driverObj as any)._cleanupSessionModal = cleanup;
        },
        onDeselected: () => {
          if ((driverObj as any)._cleanupSessionModal) {
            (driverObj as any)._cleanupSessionModal();
          }
        }
      },
      {
        element: '.tutorial-session-card',
        popover: {
          title: 'Your Recording Session',
          description: 'Your session is ready. The card shows:<ul><li>Session name and status</li><li>Hardware configuration</li><li>Episode settings</li><li>Start and Stop recording buttons</li></ul>You can now click "Start Recording" to begin capturing data. Move the leader robot and the follower will mirror it while recording.',
          side: 'top',
        }
      },
      {
        element: '.tutorial-monitor-button',
        popover: {
          title: 'Monitor Dashboard',
          description: 'Click "Monitor" to open a real-time dashboard showing:<ul><li>Live camera feeds (if configured)</li><li>Robot joint positions</li><li>Episode progress</li><li>Recording status</li><li>Data rates</li></ul>This helps verify everything is working during recording.',
          side: 'left',
        },
        onHighlighted: () => {
          const monitorButton = document.querySelector('.tutorial-monitor-button') as HTMLElement;
          if (monitorButton) {
            const handleClick = () => {
              setTimeout(() => driverObj.moveNext(), 300);
              monitorButton.removeEventListener('click', handleClick);
            };
            monitorButton.addEventListener('click', handleClick);
          }
        }
      },
      {
        popover: {
          title: 'Tutorial Complete',
          description: 'You are all set. Here is a quick recap:<ul><li>Configured leader and follower robots</li><li>Added a camera</li><li>Created producers for all hardware</li><li>Created a hardware system</li><li>Ready to record teleop sessions</li></ul>You can restart this tutorial anytime by clicking the "Tutorial" button in the navigation bar.',
        }
      },
    ],
    onDestroyStarted: () => {
      driverObj.destroy();
    },
  });

  driverObj.drive();
};
