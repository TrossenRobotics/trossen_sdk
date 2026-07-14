type BadgeVariant = 'active' | 'paused' | 'errored' | 'completed' | 'pending';

interface StatusBadgeProps {
  variant: BadgeVariant;
}

const badgeConfig = {
  active: {
    dotColor: 'bg-green-500',
    label: 'Active'
  },
  paused: {
    dotColor: 'bg-amber-500',
    label: 'Paused'
  },
  errored: {
    dotColor: 'bg-red-500',
    label: 'Errored'
  },
  completed: {
    dotColor: 'bg-blue-500',
    label: 'Completed'
  },
  pending: {
    dotColor: 'bg-gray-500',
    label: 'Pending'
  }
};

export function StatusBadge({ variant }: StatusBadgeProps) {
  const config = badgeConfig[variant];

  return (
    <div className="inline-flex items-center gap-[6px] px-[8px] py-[4px] bg-surface border border-edge">
      <div className={`w-[6px] h-[6px] rounded-full ${config.dotColor}`} />
      <span className="text-[12px] text-dim uppercase font-['JetBrains_Mono',sans-serif]">
        {config.label}
      </span>
    </div>
  );
}
