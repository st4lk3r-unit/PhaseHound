#!/bin/bash
set -x

./ph-cli sub soapy.config.out wfmd.config.out wfmd.audio-info audiosink.config.out soapy.IQ-info &

./ph-cli pub soapy.config.in "select 6"
./ph-cli pub soapy.config.in "set sr=2400000 cf=96.0e6 bw=1.5e6"
./ph-cli pub soapy.config.in "start"

./ph-cli pub wfmd.config.in "subscribe iq-source soapy.IQ-info"
./ph-cli pub soapy.config.in "open"

./ph-cli pub audiosink.config.in "subscribe pcm-source wfmd.audio-info"
./ph-cli pub wfmd.config.in "open"

./ph-cli pub wfmd.config.in "gain 0.5"
./ph-cli pub wfmd.config.in "start"
./ph-cli pub audiosink.config.in "start"

killall ph-cli
