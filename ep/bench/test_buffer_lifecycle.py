import gc
import os
from datetime import timedelta

import torch
import torch.distributed as dist

from buffer import Buffer


def test_buffer_lifecycle(group, low_latency_mode):
    buffer = Buffer(
        group,
        num_nvl_bytes=65536,
        num_rdma_bytes=65536,
        low_latency_mode=low_latency_mode,
        explicitly_destroy=True,
    )
    scratch = buffer.scratch
    dist.barrier()
    buffer.destroy()
    del buffer
    gc.collect()

    scratch.fill_(group.rank() + 1)
    torch.cuda.synchronize()
    assert torch.all(scratch == group.rank() + 1).item()
    del scratch
    gc.collect()
    torch.cuda.synchronize()
    dist.barrier()


if __name__ == "__main__":
    local_rank = int(os.environ["LOCAL_RANK"])
    torch.cuda.set_device(local_rank)
    dist.init_process_group(backend="nccl", timeout=timedelta(seconds=180))
    group = dist.new_group(list(range(dist.get_world_size())))
    for low_latency_mode in (False, True):
        test_buffer_lifecycle(group, low_latency_mode)
    dist.destroy_process_group(group)
    dist.destroy_process_group()
