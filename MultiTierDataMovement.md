# Background Data Movement

In order to reduce the number of online evictions and support asynchronous
promotion - we have added two periodic workers to handle eviction and promotion.

The diagram below shows a simplified version of how the background evictor
thread (green) is integrated to the CacheLib architecture. 

<p align="center">
  <img width="640" height="360" alt="BackgroundEvictor" src="cachelib-background-evictor.png">
</p>

## Background Evictors

The background evictors scan each class to see if there are objects to move the next (lower)
tier using a given strategy. Here we document the parameters for the different
strategies and general parameters. 

- `backgroundEvictorIntervalMilSec`: The interval that this thread runs for - by default
the background evictor threads will wake up every 10 ms to scan the AllocationClasses. Also,
the background evictor thread will be woken up everytime there is a failed allocation (from
a request handling thread) and the current percentage of free memory for the 
AllocationClass is lower than `lowEvictionAcWatermark`. This may render the interval parameter
not as important when there are many allocations occuring from request handling threads. 

- `evictorThreads`: The number of background evictors to run - each thread is a assigned
a set of AllocationClasses to scan and evict objects from. Currently, each thread gets
an equal number of classes to scan - but as object size distribution may be unequal - future
versions will attempt to balance the classes among threads. The range is 1 to number of AllocationClasses.
The default is 1. 

- `maxEvictionBatch`: The number of objects to remove in a given eviction call. The
default is 40. Lower range is 10 and the upper range is 1000. Too low and we might not
remove objects at a reasonable rate, too high and it might increase contention with user threads.

- `minEvictionBatch`: Minimum number of items to evict at any time (if there are any
candidates)

- `maxEvictionPromotionHotness`: Maximum candidates to consider for eviction. This is similar to `maxEvictionBatch`
but it specifies how many candidates will be taken into consideration, not the actual number of items to evict.
This option can be used to configure duration of critical section on LRU lock.


### FreeThresholdStrategy (default)

- `lowEvictionAcWatermark`: Triggers background eviction thread to run
when this percentage of the AllocationClass is free. 
The default is `2.0`, to avoid wasting capacity we don't set this above `10.0`.

- `highEvictionAcWatermark`: Stop the evictions from an AllocationClass when this 
percentage of the AllocationClass is free. The default is `5.0`, to avoid wasting capacity we
don't set this above `10`.


## Background Promoters

The background promoters scan each class to see if there are objects to move to a lower
tier using a given strategy. Here we document the parameters for the different
strategies and general parameters.

- `backgroundPromoterIntervalMilSec`: The interval that this thread runs for - by default
the background promoter threads will wake up every 10 ms to scan the AllocationClasses for
objects to promote.

- `promoterThreads`: The number of background promoters to run - each thread is a assigned
a set of AllocationClasses to scan and promote objects from. Currently, each thread gets
an equal number of classes to scan - but as object size distribution may be unequal - future
versions will attempt to balance the classes among threads. The range is `1` to number of AllocationClasses. The default is `1`.

- `maxProtmotionBatch`: The number of objects to promote in a given promotion call. The
default is 40. Lower range is 10 and the upper range is 1000. Too low and we might not
remove objects at a reasonable rate, too high and it might increase contention with user threads. 

- `minPromotionBatch`: Minimum number of items to promote at any time (if there are any
candidates)

- `numDuplicateElements`: This allows us to promote items that have existing handles (read-only) since
we won't need to modify the data when a user is done with the data. Therefore, for a short time
the data could reside in both tiers until it is evicted from its current tier. The default is to
not allow this (0). Setting the value to 100 will enable duplicate elements in tiers.

### Background Promotion Strategy (only one currently)

- `promotionAcWatermark`: Promote items if there is at least this
percent of free AllocationClasses. Promotion thread will attempt to move `maxPromotionBatch` number of objects
to that tier. The objects are chosen from the head of the LRU. The default is `4.0`.
This value should correlate with `lowEvictionAcWatermark`, `highEvictionAcWatermark`, `minAcAllocationWatermark`, `maxAcAllocationWatermark`.
- `maxPromotionBatch`: The number of objects to promote in batch during BG promotion. Analogous to
`maxEvictionBatch`. It's value should be lower to decrease contention on hot items.

