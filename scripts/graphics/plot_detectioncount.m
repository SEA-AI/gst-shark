#! /usr/bin/octave -qf

[pad_name_list, timestamp_mat, count_mat] = load_serie_timestamp_value('detectioncount.mat');

if ((1 == length(timestamp_mat)) && (0 == timestamp_mat))
    return
end

tracer.detectioncount.timestamp_mat = timestamp_mat;
tracer.detectioncount.count_mat = count_mat;
tracer.detectioncount.pad_name_list = pad_name_list;
