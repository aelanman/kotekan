import pytest
import numpy as np

import visbuffer
import kotekan_runner

trunc_params = {
    'fakevis_mode': 'gaussian',
    'cadence': 2.,
    'total_frames': 10,
    'err_sq_lim': 0.003,
    'weight_fixed_precision': 0.001,
    'data_fixed_precision': 0.0001,
    'num_ev': 4,
    'num_elements': 4,
    'out_file': '/tmp/out.csv'
}

@pytest.fixture(scope="module")
def vis_data(tmpdir_factory):
    """ Truncated visibilities """

    tmpdir = tmpdir_factory.mktemp("vis_data_t")

    fakevis_buffer = kotekan_runner.FakeVisBuffer(
            num_frames=trunc_params['total_frames'],
            mode=trunc_params['fakevis_mode'],
            cadence=trunc_params['cadence']);

    in_dump_config = trunc_params.copy()
    in_dump_config['base_dir'] = str(tmpdir)
    in_dump_config['file_name'] = 'fakevis'
    in_dump_config['file_ext'] = 'dump'

    out_dump_buffer = kotekan_runner.DumpVisBuffer(str(tmpdir))

    test = kotekan_runner.KotekanProcessTester(
        'visTruncate', trunc_params,
        buffers_in = fakevis_buffer,
        buffers_out = out_dump_buffer,
        global_config = trunc_params,
        parallel_process_type = 'rawFileWrite',
        parallel_process_config = in_dump_config
    )

    test.run()

    yield (out_dump_buffer.load(), visbuffer.VisBuffer.load_files(
        "%s/*fakevis*.dump" % str(tmpdir)))

@pytest.fixture(scope="module")
def vis_data_zero_weights(tmpdir_factory):
    """ Truncated visibilities """

    tmpdir = tmpdir_factory.mktemp("vis_data_t")

    fakevis_buffer = kotekan_runner.FakeVisBuffer(
            num_frames=trunc_params['total_frames'],
            mode=trunc_params['fakevis_mode'],
            cadence=trunc_params['cadence'],
            zero_weight=True);

    in_dump_config = trunc_params.copy()
    in_dump_config['base_dir'] = str(tmpdir)
    in_dump_config['file_name'] = 'fakevis'
    in_dump_config['file_ext'] = 'dump'

    out_dump_buffer = kotekan_runner.DumpVisBuffer(str(tmpdir))

    test = kotekan_runner.KotekanProcessTester(
        'visTruncate', trunc_params,
        buffers_in = fakevis_buffer,
        buffers_out = out_dump_buffer,
        global_config = trunc_params,
        parallel_process_type = 'rawFileWrite',
        parallel_process_config = in_dump_config
    )

    test.run()

    yield (out_dump_buffer.load(), visbuffer.VisBuffer.load_files(
        "%s/*fakevis*.dump" % str(dir)))

def test_truncation(vis_data):
    n = trunc_params['num_elements']

    for frame_t, frame in zip(vis_data[0], vis_data[1]):
        assert np.any(frame.vis != frame_t.vis)
        assert np.all(np.abs(frame.vis - frame_t.vis)
                      <= np.sqrt(trunc_params['err_sq_lim'] / frame.weight))
        assert np.any(frame.weight != frame_t.weight)
        assert np.all(np.abs(frame.weight - frame_t.weight)
                      <= np.abs(frame.weight) * trunc_params['weight_fixed_precision'])
        assert np.all(np.abs(frame.evec.real - frame_t.evec.real)
                      <= np.abs(frame.evec.real) * trunc_params['data_fixed_precision'])
        assert np.all(np.abs(frame.evec.imag - frame_t.evec.imag)
                      <= np.abs(frame.evec.imag) * trunc_params['data_fixed_precision'])

        # test if RMSE of vis is within 5 sigma of sqrt(3 err_sq_lim * average weight)
        rmse = np.sqrt(np.mean(np.abs((frame.vis - frame_t.vis)**2)))
        expected_rmse = np.sqrt(trunc_params['err_sq_lim'] / (3 * np.abs(frame.weight)))
        five_sigma = 5 * expected_rmse / np.sqrt(len(frame.vis))
        assert np.all(np.abs(rmse - expected_rmse) < five_sigma)

def test_zero_weights(vis_data_zero_weights):
    n = trunc_params['num_elements']

    for frame_t, frame in zip(vis_data_zero_weights[0], vis_data_zero_weights[1]):
        assert np.any(frame.vis != frame_t.vis)
        for i in range(0, int(n * (n+1) * 0.5)):
            assert (np.abs(frame.vis[i].real - frame_t.vis[i].real)
                          <= np.abs(frame.vis[i].real) * trunc_params['data_fixed_precision'])
            assert (np.abs(frame.vis[i].imag - frame_t.vis[i].imag)
                          <= np.abs(frame.vis[i].imag) * trunc_params['data_fixed_precision'])
        assert np.all(frame.weight == frame_t.weight)
        assert np.all(frame.weight == 0.0)
        assert np.all(np.abs(frame.evec.real - frame_t.evec.real)
                      <= np.abs(frame.evec.real) * trunc_params['data_fixed_precision'])
        assert np.all(np.abs(frame.evec.imag - frame_t.evec.imag)
                      <= np.abs(frame.evec.imag) * trunc_params['data_fixed_precision'])