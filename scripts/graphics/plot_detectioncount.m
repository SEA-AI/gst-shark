#! /usr/bin/octave -qf

[det_pad_list, det_ts_mat, det_cnt_mat] = load_serie_timestamp_value('detectioncount_detector.mat');
[trk_pad_list, trk_ts_mat, trk_cnt_mat] = load_serie_timestamp_value('detectioncount_tracker.mat');

has_det = !((1 == length(det_ts_mat)) && (0 == det_ts_mat));
has_trk = !((1 == length(trk_ts_mat)) && (0 == trk_ts_mat));

if (!has_det && !has_trk)
    return
end

if (has_det)
    tracer.detectioncount_detector.timestamp_mat = det_ts_mat;
    tracer.detectioncount_detector.count_mat = det_cnt_mat;
    tracer.detectioncount_detector.pad_name_list = det_pad_list;
end

if (has_trk)
    tracer.detectioncount_tracker.timestamp_mat = trk_ts_mat;
    tracer.detectioncount_tracker.count_mat = trk_cnt_mat;
    tracer.detectioncount_tracker.pad_name_list = trk_pad_list;
end
