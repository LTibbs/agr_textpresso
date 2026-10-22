#
# Build:
#	docker build -f Dockerfile-cloud -t cloud-tpc .
#
FROM ubuntu-tpc

ENV DEBIAN_FRONTEND noninteractive

# install libraries and stuff, reconfigure system
RUN apt-get update; apt-get install -y -qq libfcgi-dev lighttpd sudo gdb pigz rsync postfix libopenblas-dev rsyslog
RUN sed -i -e 's/# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen && dpkg-reconfigure --frontend=noninteractive locales && update-locale LANG=en_US.UTF-8
ENV LANG en_US.UTF-8 
ENV LC_ALL en_US.UTF-8

# Copy essential files
COPY main.cf /etc/postfix/main.cf
COPY sasl_passwd /etc/postfix/sasl/sasl_passwd
RUN postmap /etc/postfix/sasl/sasl_passwd
RUN chfn -f "Textpresso Central" root
COPY initialize.sh /root/initialize.sh
COPY start_textpresso.sh /root/start_textpresso.sh
COPY lighttpd.conf /etc/lighttpd/lighttpd.conf
COPY crontab /usr/local/etc/

RUN git clone https://github.com/ipkn/crow.git
RUN git clone https://github.com/SRombauts/SQLiteCpp.git; cd SQLiteCpp; git checkout 3.3.1; mkdir cmake-build-release; cd cmake-build-release; cmake ..; make -j 8; make install; rm -rf /SQLiteCpp

# Precompile system
COPY textpressocentral /data/textpresso/textpressocentral
COPY textpressoapi /data/textpresso/textpressoapi
COPY tpctools /data/textpresso/tpctools
RUN /root/initialize.sh -t \
&& rm -rf /data/textpresso/textpressocentral \
&& rm -rf /data/textpresso/textpressoapi \
&& rm -rf /data/textpresso/textpressoapi_data \
&& rm -rf /data/textpresso/tpctools

# essential files and dirs
WORKDIR /usr/local/textpresso
RUN ln -s ../uima_descriptors descriptors
RUN mkdir -p uti
RUN mkdir -p etc
COPY stopwords.postgres.tar.gz /usr/local/textpresso/etc/.
WORKDIR /

RUN ARCH="$(dpkg --print-architecture)" \
 && if [ "$ARCH" = "arm64" ]; then MC_ARCH="aarch64"; else MC_ARCH="x86_64"; fi \
 && MC_SH="Miniconda3-py311_25.5.1-0-Linux-${MC_ARCH}.sh" \
 && wget -q "https://repo.anaconda.com/miniconda/${MC_SH}" \
 && bash "${MC_SH}" -b \
 && rm "${MC_SH}"
ENV PATH="${PATH}:/root/miniconda3/bin"
RUN conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/main
RUN conda tos accept --override-channels --channel https://repo.anaconda.com/pkgs/r
ADD conda_env.yml conda_env.yml
RUN conda env create -f conda_env.yml
SHELL ["conda", "run", "-n", "agr_textpresso", "/bin/bash", "-c"]
ADD requirements.txt requirements.txt
RUN conda run -n agr_textpresso pip install -r requirements.txt

# start cron
RUN touch /var/log/cron.log && crontab /usr/local/etc/crontab

CMD /root/start_textpresso.sh
#    RUN:
#
#    see run_tpc_cloud.sh in this directory
