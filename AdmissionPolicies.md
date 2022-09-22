# Admission Policies

## Allocation policies

- `maxAcAllocationWatermark`:  Item is always allocated in topmost tier if at least this 
percentage of the AllocationClass is free.
- `minAcAllocationWatermark`: Item is always allocated in bottom tier if only this percent
of the AllocationClass is free. If percentage of free AllocationClasses is between `maxAcAllocationWatermark`
and `minAcAllocationWatermark`: then extra checks (described below) are performed to decide where to put the element.

By default, allocation will always be performed from the upper tier.

- `acTopTierEvictionWatermark`: If there is less that this percent of free memory in topmost tier, cachelib will attempt to evict from top tier. This option takes precedence before allocationWatermarks.

### Extra policies (used only when  percentage of free AllocationClasses is between `maxAcAllocationWatermark`
and `minAcAllocationWatermark`)
- `sizeThresholdPolicy`: If item is smaller than this value, always allocate it in upper tier.
- `defaultTierChancePercentage`: Change (0-100%) of allocating item in top tier

## MMContainer options

- `lruInsertionPointSpec`: Can be set per tier when LRU2Q is used. Determines where new items are
inserted. 0 = insert to hot queue, 1 = insert to warm queue, 2 = insert to cold queue
- `markUsefulChance`: Per-tier, determines chance of moving item to the head of LRU on access
