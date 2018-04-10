
import pytest
import numpy as np
import h5py

import kotekan_runner

writer_params = {
    'num_elements': 4,
    'num_ev': 2,
    'cadence': 5.0,
    'total_frames': 10,  # One extra sample to ensure we actually get 256
    'freq': [3, 777, 554],
    'write_freq':[3, 777]
}

def written_data_base(write_ev, outdir):

    fakevis_buffer = kotekan_runner.FakeVisBuffer(
        freq_ids=writer_params['freq'],
        num_frames=writer_params['total_frames'],
        cadence=writer_params['cadence']
    )

    params = writer_params.copy()
    params['root_path'] = outdir

    test = kotekan_runner.KotekanProcessTester(
        'visWriter', {'freq_ids': params['write_freq'], 'node_mode': False,
        'write_ev': write_ev},
        fakevis_buffer,
        None,
        params
    )

    test.run()

    import glob

    files = sorted(glob.glob(outdir + '/20??????T??????Z_*_corr/*.h5'))

    return [h5py.File(fname, 'r') for fname in files]

@pytest.fixture(scope="module")
def written_data(request, tmpdir_factory):

    tmpdir = tmpdir_factory.mktemp("writer")

    fhlist = written_data_base(False, str(tmpdir))

    yield fhlist

    for fh in fhlist:
        fh.close()

@pytest.fixture(scope="module")
def written_data_ev(request, tmpdir_factory):

    tmpdir = tmpdir_factory.mktemp("writer_ev")

    fhlist = written_data_base(True, str(tmpdir))

    yield fhlist

    for fh in fhlist:
        fh.close()


def test_vis(written_data):

    nt = writer_params['total_frames']

    for fh in written_data:
        vis = fh['vis'][:nt]

        # Check the diagonals are correct
        pi = 0
        for ii in range(writer_params['num_elements']):
            assert (vis[:, :, pi].imag == ii).all()
            pi += writer_params['num_elements'] - ii

        # Check the times are correct
        ftime = fh['index_map/time']['fpga_count'][:nt].astype(np.float32)
        ctime = fh['index_map/time']['ctime'][:nt].astype(np.float32)
        assert (vis[:, :, 0].real == ftime[:, np.newaxis]).all()
        assert (vis[:, :, 1].real == ctime[:, np.newaxis]).all()

        # Check the frequencies are correct
        freq = fh['index_map/freq']['centre']
        vfreq = (800.0 - 400.0 * vis[:, :, 2].real / 1024)
        assert (vfreq == freq[np.newaxis, :]).all()


def test_metadata(written_data):

    nt = writer_params['total_frames']

    for fh in written_data:

        # Check the number of samples has been written correctly
        assert fh.attrs['num_time'] == nt

        # Check the times
        ctime = fh['index_map/time']['ctime'][:nt]
        assert np.allclose(np.diff(ctime), writer_params['cadence'])

        # Check the frequencies
        freq = fh['index_map/freq']['centre']
        wfreq = 800.0 - 400.0 * np.array(writer_params['write_freq']) / 1024
        assert (freq == wfreq).all()

        # Check the products
        ia, ib = np.triu_indices(writer_params['num_elements'])
        assert (fh['index_map/prod']['input_a'] == ia).all()
        assert (fh['index_map/prod']['input_b'] == ib).all()


def test_no_eigenvectors(written_data):

    for fh in written_data:
        assert 'eval' not in fh
        assert 'evec' not in fh
        assert 'erms' not in fh


def test_eigenvectors(written_data_ev):
    
    for fh in written_data_ev:
        nt = writer_params['total_frames']
        nf = len(writer_params['write_freq'])
        ne = writer_params['num_ev']
        ni = writer_params['num_elements']

        evals = fh['eval'][:nt]
        evecs = fh['evec'][:nt]
        erms = fh['erms'][:nt]

        # Check datasets are present
        assert evals.shape == (nt, nf, ne)
        assert evecs.shape == (nt, nf, ne, ni)
        assert erms.shape == (nt, nf)

        # Check that the index map is there correctly
        assert (fh['index_map/ev'][:] == np.arange(ne)).all()

        # Check that the datasets have the correct values
        assert (evals == np.arange(ne)[np.newaxis, np.newaxis, :]).all()
        assert (evecs.real == np.arange(ne)[np.newaxis, np.newaxis, :, np.newaxis]).all()
        assert (evecs.imag == np.arange(ni)[np.newaxis, np.newaxis, np.newaxis, :]).all()
        assert (erms == 1.0).all()


def test_unwritten(written_data):

    nt = writer_params['total_frames']

    for fh in written_data:

        assert (fh['vis'][nt:] == 0.0).all()
        assert (fh['flags/vis_weight'][nt:] == 0.0).all()
        assert (fh['index_map/time'][nt:]['ctime'] == 0.0).all()
        assert (fh['index_map/time'][nt:]['fpga_count'] == 0).all()