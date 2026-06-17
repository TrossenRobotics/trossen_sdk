interface EpisodeProgressProps {
  current: number;
  total: number;
  elapsedTime?: string;
}

export function EpisodeProgress({ current, total, elapsedTime }: EpisodeProgressProps) {
  const percentage = (current / total) * 100;

  return (
    <div className="space-y-[6px]">
      <div className="flex items-center justify-between">
        <div className="text-dim text-[12px] font-['JetBrains_Mono',sans-serif]">
          Episode {current} of {total}
        </div>
        {elapsedTime && (
          <div className="text-dim text-[12px] font-['JetBrains_Mono',sans-serif]">
            Elapsed: {elapsedTime}
          </div>
        )}
      </div>
      <div className="h-[8px] bg-edge border border-edge relative overflow-hidden">
        <div
          className="absolute inset-y-0 left-0 bg-brand transition-all duration-300"
          style={{ width: `${percentage}%` }}
        />
      </div>
    </div>
  );
}
