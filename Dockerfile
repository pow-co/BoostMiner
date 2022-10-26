FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update

RUN apt-get -y install python3-pip build-essential git gcc-10 g++-10 cmake

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 20

RUN update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 20

RUN pip install conan

RUN conan config set general.revisions_enabled=1
RUN conan profile new default --detect
RUN conan profile update settings.compiler.libcxx=libstdc++11 default
RUN conan remote add proofofwork https://conan.pow.co/artifactory/api/conan/pow-virtual

#gigamonkey
WORKDIR /home/conan/
RUN rm -rf Gigamonkey
RUN git clone --depth 1 --branch master https://github.com/Gigamonkey-BSV/Gigamonkey.git
WORKDIR /home/conan/Gigamonkey
RUN conan install .
RUN conan create . proofofwork/stable

COPY . /home/conan/boostminer
WORKDIR /home/conan/boostminer
RUN chmod -R 777 .

RUN conan install .
RUN conan build .

CMD ./bin/BoostMiner


