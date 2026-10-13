// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "camera_diagnostics.h"

#include <QtCore/QCoreApplication>

#include <nx/utils/log/assert.h>

class QnCameraDiagnosticsErrorCodeStrings
{
    Q_DECLARE_TR_FUNCTIONS(QnCameraDiagnosticsErrorCodeStrings)

public:
    static QString toString(
        CameraDiagnostics::ErrorCode::Value val,
        const QList<QString>& errorParams)
    {
        using namespace CameraDiagnostics::ErrorCode;

        QString detailsRebootRestore(
            tr("Please try to reboot the camera, then restore factory defaults on the web-page."));

        QString detailsPluggedReboot(
            tr("Make sure the camera is plugged into the network. Try to reboot the camera."));

        QString firmwareSupport = tr(
            "Finally, try to update firmware. If the problem persists, please contact support.");

        auto getParam = [&errorParams](int index)
        {
            QString result;
            if (errorParams.size() > index)
                result = errorParams[index];

            return result.isEmpty() ? tr("(unknown)") : result;
        };

        // Pre-cache to simplify format strings
        const QString p1 = getParam(0);
        const QString p2 = getParam(1);

        QStringList errorMessageParts;
        switch (val)
        {
            case noError:
            {
                errorMessageParts << tr("OK");
                break;
            }
            case mediaServerUnavailable:
            {
                errorMessageParts << tr("Server %1 is not available.").arg(p1)
                                  << tr("Check that Server is up and running.");
                break;
            }
            case mediaServerBadResponse:
            {
                errorMessageParts
                    << tr("Received bad response from Server %1: \"%2\".").arg(p1).arg(p2)
                    << tr("Check if Server is up and has the proper version.");
                break;
            }
            case cannotEstablishConnection:
            {
                errorMessageParts << tr("Cannot connect to http port %1.").arg(p1)
                                  << detailsPluggedReboot;
                break;
            }
            case cannotOpenCameraMediaPort:
            {
                errorMessageParts
                    << tr("Cannot open media URL %1. Failed to connect to media port %2.")
                           .arg(p1)
                           .arg(p2)
                    << tr("Make sure port %1 is accessible (e.g. forwarded).").arg(p2)
                    << detailsRebootRestore;
                break;
            }
            case liveVideoIsNotSupportedError:
            {
                errorMessageParts << tr(
                    "Camera was restored from archive. Delete the camera and add it again to view Live video.");
                break;
            }
            case connectionClosedUnexpectedly:
            {
                errorMessageParts
                    << tr("Cannot open media URL %1. Connection to port %2 was closed unexpectedly.")
                           .arg(p1)
                           .arg(p2)
                    << detailsPluggedReboot;
                break;
            }
            case responseParseError:
            {
                QString detailsBase(tr("Could not parse camera response. URL %1, request name %2.")
                                        .arg(p1)
                                        .arg(p2));

                errorMessageParts << detailsBase << detailsRebootRestore << firmwareSupport;
                break;
            }
            case noMediaTrack:
            {
                errorMessageParts << tr("No supported media tracks at URL %1.").arg(p1)
                                  << detailsRebootRestore << firmwareSupport;
                break;
            }
            case notAuthorised:
            {
                errorMessageParts << tr("Not authorized. URL %1.").arg(p1);
                break;
            }
            case unsupportedProtocol:
            {
                errorMessageParts << tr("Cannot open media URL %1. Unsupported media protocol %2.")
                                         .arg(p1)
                                         .arg(p2)
                                  << detailsRebootRestore << firmwareSupport;
                break;
            }
            case cannotConfigureMediaStream:
            {
                QString detailsBase(
                    tr("If disabled, try to enable Recording and decrease FPS in Camera Settings "
                       "(error \"%1\").")
                        .arg(p1));
                QString detailsAdvanced(
                    tr("If it does not help, restore factory defaults on the camera web-page."));

                errorMessageParts << tr("Failed to configure parameter %1.").arg(p1) << detailsBase
                                  << detailsAdvanced << firmwareSupport;
                break;
            }
            case requestFailed:
            {
                QString detailsBase(
                    tr("Camera request \"%1\" failed with error \"%2\".").arg(p1).arg(p2));
                errorMessageParts << detailsBase << detailsRebootRestore << firmwareSupport;
                break;
            }
            case notImplemented:
            {
                QString details(tr("Unknown camera issue."));

                errorMessageParts << details << tr("Please contact support.");
                break;
            }
            case ioError:
            {
                errorMessageParts
                    << tr("An input/output error has occurred. OS message: \"%1\".").arg(p1)
                    << detailsPluggedReboot;
                break;
            }
            case serverTerminated:
            {
                errorMessageParts << tr("Server has been stopped.");
                break;
            }
            case cameraInvalidParams:
            {
                QString details(
                    tr("Invalid data was received from the camera. Details: \"%1\".").arg(p1));

                errorMessageParts << details;
                break;
            }
            case cameraOldFirmwareError:
            {
                errorMessageParts << tr("Please update firmware. "
                                        "Minimal supported version is %1. Current version is %2")
                                         .arg(p1)
                                         .arg(p2);
                break;
            }
            case badMediaStream:
            {
                QString details(tr(
                    "Too many media errors. Please open camera issues dialog for more details."));

                errorMessageParts << details;
                break;
            }
            case noMediaStream:
            {
                errorMessageParts << tr("Media stream is opened but no media data was received.");
                break;
            }
            case initializationInProgress:
            {
                QString details(tr("Camera initialization process is in progress."));

                errorMessageParts << details;
                break;
            }
            case pluginError:
            {
                errorMessageParts << tr("Plugin error. %1").arg(p1);
                break;
            }
            case pluginIsUnavailable:
            {
                errorMessageParts << tr("Plugin is unavailable");
                break;
            }
            case internalServerError:
            {
                errorMessageParts << tr("Internal server error. %1").arg(p1);
                break;
            }
            default:
            {
                QStringList nonEmptyParams = errorParams;
                nonEmptyParams.removeAll(QString());

                errorMessageParts << tr("Unknown error. Please contact support.");
                if (!nonEmptyParams.isEmpty())
                {
                    errorMessageParts << QString() << QString() << tr("Parameters:")
                                      << nonEmptyParams.join(", ");
                }
                break;
            }
        }

        return errorMessageParts.join('\n');
    }
};

namespace CameraDiagnostics {

namespace ErrorCode {

QString toString(Value val, const ErrorParams& errorParams)
{
    return QnCameraDiagnosticsErrorCodeStrings::toString(val, errorParams);
}

QString toString(int val, const ErrorParams& errorParams)
{
    return toString(static_cast<Value>(val), errorParams);
}

} // namespace ErrorCode

Result::Result():
    errorCode(ErrorCode::noError)
{
}

Result::Result(ErrorCode::Value _errorCode, const QString& param1, const QString& param2):
    errorCode(_errorCode)
{
    errorParams.push_back(param1);
    errorParams.push_back(param2);
}

Result::operator bool() const
{
    return errorCode == ErrorCode::noError;
}

QString Result::toString() const
{
    return ErrorCode::toString(errorCode, errorParams);
}

QString Result::urlToStr(const nx::Url& value) const
{
    return value.toString(QUrl::RemoveUserInfo);
}

} // namespace CameraDiagnostics
