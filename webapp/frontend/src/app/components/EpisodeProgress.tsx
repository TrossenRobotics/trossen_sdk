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
        <div className="text-[#b9b8ae] text-[12px] font-['JetBrains_Mono',sans-serif]">
          Episode {current} of {total}
        </div>
        {elapsedTime && (
          <div className="text-[#b9b8ae] text-[12px] font-['JetBrains_Mono',sans-serif]">
            Elapsed: {elapsedTime}
          </div>
        )}
      </div>
      <div className="h-[8px] bg-[#252525] border border-[#252525] relative overflow-hidden">
        <div
          className="absolute inset-y-0 left-0 bg-[#55bde3] transition-all duration-300"
          style={{ width: `${percentage}%` }}
        />
      </div>
    </div>
  );
}
