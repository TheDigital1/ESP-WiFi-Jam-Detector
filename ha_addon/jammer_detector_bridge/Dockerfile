# SPDX-License-Identifier: GPL-3.0-or-later
# Built by the Supervisor on the device (no published image). Since Supervisor
# 2026.04 BUILD_FROM is not passed by default, so the base is named here.
ARG BUILD_FROM=ghcr.io/home-assistant/base-python:3.14-alpine3.24
FROM ${BUILD_FROM}

COPY requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt && rm /tmp/requirements.txt

COPY jdbridge /opt/jdbridge/jdbridge
COPY run.sh /run.sh
RUN chmod a+x /run.sh

ENV PYTHONPATH=/opt/jdbridge PYTHONUNBUFFERED=1
CMD [ "/run.sh" ]
