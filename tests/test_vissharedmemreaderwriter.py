# === Start Python 2/3 compatibility
from __future__ import absolute_import, division, print_function, unicode_literals
from future.builtins import *  # noqa pylint: disable=W0401, W0614
from future.builtins.disabled import *  # noqa pylint: disable=W0401, W0614

# == End Python 2/3 compatibility

import logging
import numpy as np
import os
import posix_ipc
import pytest
import re
import signal
from subprocess import Popen
import shutil
import tempfile
import threading
from time import sleep

from comet import Manager

from kotekan import runner, shared_memory_buffer

sem_name = "kotekan"
fname_buf = "calBuffer"

logging.basicConfig(level=logging.DEBUG)


@pytest.fixture()
def comet_broker():
    broker_path = shutil.which("comet")
    if not broker_path:
        pytest.skip(
            "Make sure PYTHONPATH is set to where the comet dataset broker is installed."
        )

    with tempfile.NamedTemporaryFile(mode="w") as f_out:
        # Start comet with a random port
        broker = Popen(
            [broker_path, "-p", "0", "--recover", "False"], stdout=f_out, stderr=f_out
        )
        sleep(3)

        # Find port in the log
        regex = re.compile("Selected random port: ([0-9]+)$")
        log = open(f_out.name, "r").read().split("\n")
        port = None
        for line in log:
            print(line)
            match = regex.search(line)
            if match:
                port = match.group(1)
                print("Test found comet port in log: %s" % port)
                break
        if not match:
            print("Could not find comet port in logs.")
            exit(1)

        try:
            yield port
        finally:
            pid = broker.pid
            os.kill(pid, signal.SIGINT)
            broker.terminate()
            log = open(f_out.name, "r").read().split("\n")
            for line in log:
                print(line)


@pytest.fixture(scope="module")
def semaphore():
    sem = posix_ipc.Semaphore(sem_name)
    yield sem
    sem.release()
    sem.unlink()


params = {
    "num_elements": 7,
    "num_ev": 0,
    "total_frames": 11,
    "cadence": 1.0,
    "mode": "default",
    "dataset_manager": {"use_dataset_broker": True},
}

params_fakevis = {
    "freq_ids": [1, 2, 3, 4, 7, 10],
    "num_frames": params["total_frames"],
    "mode": params["mode"],
    "wait": True,
}

params_writer_stage = {"nsamples": 5}


@pytest.fixture()
def vis_data_slow(tmpdir_factory, comet_broker):

    # keeping all the data this test produced here (probably do not need it)
    # using FakeVisBuffer to produce fake data
    fakevis_buffer = runner.FakeVisBuffer(**params_fakevis)

    # pass comet port to kotekan
    params["dataset_manager"]["ds_broker_port"] = comet_broker

    # KotekanStageTester is used to run kotekan with my config
    test = runner.KotekanStageTester(
        stage_type="visSharedMemWriter",
        stage_config=params_writer_stage,
        buffers_in=fakevis_buffer,
        buffers_out=None,
        global_config=params,
    )
    yield test


def test_shared_mem_buffer(vis_data_slow, comet_broker):
    num_freq = len(params_fakevis["freq_ids"])
    num_ev = params["num_ev"]
    num_elements = params["num_elements"]

    threading.Thread(target=vis_data_slow.run).start()
    sleep(2)
    buffer = shared_memory_buffer.SharedMemoryReader(sem_name, fname_buf, 4)

    assert buffer.num_time == params_writer_stage["nsamples"]
    assert buffer.num_freq == num_freq

    # access_record = []
    # for t in
    # assert buffer._access_record() == access_record

    n_times_to_read = 3

    ds_manager = Manager("localhost", comet_broker)

    i = 0
    with pytest.raises(shared_memory_buffer.SharedMemoryError):
        while True:
            sleep(0.5)
            print(buffer._access_record())
            visraw = buffer.read_last(n_times_to_read)
            assert visraw.num_freq == len(params_fakevis["freq_ids"])
            assert visraw.num_time == n_times_to_read

            ds = np.array(visraw.metadata["dataset_id"]).copy().view("u8,u8")
            unique_ds = np.unique(ds)
            for ds in unique_ds:
                print("dataset ID: {:x}{:x}".format(ds[1], ds[0]))

            evals = visraw.data["eval"]
            evecs = visraw.data["evec"]
            erms = visraw.data["erms"]

            # Check datasets are present
            assert evals.shape == (n_times_to_read, num_freq, num_ev)
            assert evecs.shape == (n_times_to_read, num_freq, num_ev * num_elements)
            assert erms.shape == (n_times_to_read, num_freq)

            evecs = evecs.reshape(n_times_to_read, num_freq, num_ev, num_elements)

            # Check that the datasets have the correct values
            assert (evals == np.arange(num_ev)[np.newaxis, np.newaxis, :]).all()
            assert (
                evecs.real == np.arange(num_ev)[np.newaxis, np.newaxis, :, np.newaxis]
            ).all()
            assert (
                evecs.imag
                == np.arange(num_elements)[np.newaxis, np.newaxis, np.newaxis, :]
            ).all()
            # assert (erms == 1.0).all()

            i += 1
    assert i >= 2


def test_shared_mem_buffer_read_since(vis_data_slow):
    num_freq = len(params_fakevis["freq_ids"])
    num_ev = params["num_ev"]
    num_elements = params["num_elements"]

    threading.Thread(target=vis_data_slow.run).start()
    sleep(2)
    buffer = shared_memory_buffer.SharedMemoryReader(sem_name, fname_buf, 4)

    assert buffer.num_time == params_writer_stage["nsamples"]
    assert buffer.num_freq == num_freq

    # access_record = []
    # for t in
    # assert buffer._access_record() == access_record

    timestamp = 0

    i = 0
    with pytest.raises(shared_memory_buffer.SharedMemoryError):
        while True:
            sleep(0.5)
            print(buffer._access_record())
            visraw = buffer.read_since(timestamp)

            if visraw is not None:
                timestamp = visraw.time[-1][0]
                print("Next timestamp is {}.".format(timestamp))

                assert visraw.num_freq == len(params_fakevis["freq_ids"])
                num_time = visraw.num_time
                print("num times read: {}".format(num_time))

                ds = np.array(visraw.metadata["dataset_id"]).copy().view("u8,u8")
                unique_ds = np.unique(ds)

                evals = visraw.data["eval"]
                evecs = visraw.data["evec"]
                erms = visraw.data["erms"]

                # Check datasets are present
                assert evals.shape == (num_time, num_freq, num_ev)
                assert evecs.shape == (num_time, num_freq, num_ev * num_elements)
                assert erms.shape == (num_time, num_freq)

                evecs = evecs.reshape(num_time, num_freq, num_ev, num_elements)

                # Check that the datasets have the correct values
                assert (evals == np.arange(num_ev)[np.newaxis, np.newaxis, :]).all()
                assert (
                    evecs.real
                    == np.arange(num_ev)[np.newaxis, np.newaxis, :, np.newaxis]
                ).all()
                assert (
                    evecs.imag
                    == np.arange(num_elements)[np.newaxis, np.newaxis, np.newaxis, :]
                ).all()
                # assert (erms == 1.0).all()

            i += 1
    assert i >= 2
