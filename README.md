# DISCONTINUATION OF PROJECT #
This project will no longer be maintained by Intel.
Intel has ceased development and contributions including, but not limited to, maintenance, bug fixes, new releases, or updates, to this project.
Intel no longer accepts patches to this project.
# Multi-Adapter-Particles
Demonstration of Integrated + Discrete Multi-Adapter modified from Microsoft's D3D12nBodyGravity

This demo shows a best-known-method for a game developer to add the power of integrated graphics to a platform that primarily uses discrete graphics for rendering.

Typically we expect the discrete device to have substantially more compute resources than the integrated device: after all, it has dedicated memory, a much larger power budget, much more thermal headroom, and likely more raw silicon. Nevertheless, even modest integrated GPUs represent multiple CPU cores worth of compute capability. If your application has tasks that can be done asynchronously (like particle simulation, a common asyc compute task) it may be reasonable to move that task to the (otherwise idle) integrated device. In that case, you must pay attention to where resources are allocated, and should leverage a copy queue to parallelize the data migration from host memory to discrete local memory.

Amdahl's law tells you this is not always going to be a win. As a rule of thumb, platforms where the discrete device is less than 10x the performance of the integrated are good candidates for this technique. For example, an Intel HD620 provides a capable companion to an AMD RX550, adding roughly 33% more total peak compute (~400GF + ~1.2TF). In my testing with an HD620 Intel iGPU and an AMD RX480 (~400GF + ~4TF), I was still able to measure a modest performance improvement; however, at this point, 1ms of compute on the discrete device now takes 10ms to compute on the integrated device, so we are approaching the limits of what might fit in a 60FPS frame budget. With Intel Gen11 GPUs providing ~1TF @ 1GHz and Intel Xe GPUs launching soon with even stronger statistics, integrated GPUs are becoming ever more attractive companions in platforms that also contain discrete GPUs.

August 31, 2020 Update: When the same GPU is selected for both render and compute tasks, the sample now uses async compute (the compute and render tasks share the same resources with no additional copy). This provides a fair performance comparison, and demonstrates how a single application can use both techniques. The main source of complexity in this application was the copies to enable dynamic switching of devices, which would not occur in a "real" application. More could be done to simplify this, TBD maybe :) The runtime changes are simply to the resources used and the fences for synchronization.
