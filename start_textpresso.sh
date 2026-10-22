#!/usr/bin/env bash
/bin/bash -c 'declare -p' | grep -Ev 'BASHOPTS|BASH_VERSINFO|EUID|PPID|SHELLOPTS|UID' > /container.env
cron
mkdir -p /data/textpresso/textpressoapi_data
mkdir -p /data/textpresso/postgres
ln -s /usr/local/bin/tai.sh /usr/local/bin/convert_text
ln -s /usr/local/bin/03jats2cas.sh /usr/local/bin/jats2cas
ln -s /usr/local/bin/03pdf2cas4tai.sh /usr/local/bin/tokenize
ln -s /usr/local/bin/07cas1tocas2.sh /usr/local/bin/annotate
ln -s /usr/local/bin/12index.sh /usr/local/bin/index
/root/initialize.sh -p
createdb "www-data"; zcat /usr/local/textpresso/etc/stopwords.postgres.tar.gz | pg_restore -d "www-data"
/root/initialize.sh -l &
/root/initialize.sh -w
/root/initialize.sh -i
textpressoapi -d /data/textpressoapi_data/tokens.db > /data/textpressoapi_data/api.log
nohup python3 /usr/local/textpresso/cas_annotate/cas_annotate_server.py \
    > /data/textpresso/textpressoapi_data/cas_annotate.log 2>&1 &