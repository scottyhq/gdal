/******************************************************************************
 *
 * Project:  Interlis 2 Translator
 * Purpose:  Implements OGRILI2DataSource class.
 * Author:   Markus Schnider, Sourcepole AG
 *
 ******************************************************************************
 * Copyright (c) 2004, Pirmin Kalberer, Sourcepole AG
 * Copyright (c) 2007-2008, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_conv.h"
#include "cpl_string.h"

#include "ili2reader.h"

#include "ogr_ili2.h"

using namespace std;

/************************************************************************/
/*                         OGRILI2DataSource()                         */
/************************************************************************/

OGRILI2DataSource::OGRILI2DataSource()
    : pszName(nullptr), poImdReader(new ImdReader(2)), poReader(nullptr),
      fpOutput(nullptr), nLayers(0), papoLayers(nullptr)
{
}

/************************************************************************/
/*                        ~OGRILI2DataSource()                         */
/************************************************************************/

OGRILI2DataSource::~OGRILI2DataSource()

{
    for (int i = 0; i < nLayers; i++)
    {
        delete papoLayers[i];
    }
    CPLFree(papoLayers);

    if (fpOutput != nullptr)
    {
        VSIFPrintfL(fpOutput, "</%s>\n", poImdReader->mainBasketName.c_str());
        VSIFPrintfL(fpOutput, "</DATASECTION>\n");
        VSIFPrintfL(fpOutput, "</TRANSFER>\n");
        VSIFCloseL(fpOutput);
    }

    DestroyILI2Reader(poReader);
    delete poImdReader;
    CPLFree(pszName);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRILI2DataSource::Open(const char *pszNewName, char **papszOpenOptionsIn,
                            int bTestOpen)

{
    CPLString osBasename;
    CPLString osModelFilename;

    if (CSLFetchNameValue(papszOpenOptionsIn, "MODEL") != nullptr)
    {
        osBasename = pszNewName;
        osModelFilename = CSLFetchNameValue(papszOpenOptionsIn, "MODEL");
    }
    else
    {
        char **filenames = CSLTokenizeString2(pszNewName, ",", 0);
        int nCount = CSLCount(filenames);
        if (nCount == 0)
        {
            CSLDestroy(filenames);
            return FALSE;
        }
        osBasename = filenames[0];

        if (nCount > 1)
            osModelFilename = filenames[1];

        CSLDestroy(filenames);
    }

    pszName = CPLStrdup(osBasename);

    /* -------------------------------------------------------------------- */
    /*      Open the source file.                                           */
    /* -------------------------------------------------------------------- */
    VSILFILE *fp = VSIFOpenL(pszName, "r");
    if (fp == nullptr)
    {
        if (!bTestOpen)
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to open ILI2 file `%s'.", pszNewName);

        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      If we aren't sure it is ILI2, load a header chunk and check     */
    /*      for signs it is ILI2                                            */
    /* -------------------------------------------------------------------- */
    char szHeader[1000];
    if (bTestOpen)
    {
        int nLen =
            static_cast<int>(VSIFReadL(szHeader, 1, sizeof(szHeader), fp));
        if (nLen == sizeof(szHeader))
            szHeader[sizeof(szHeader) - 1] = '\0';
        else
            szHeader[nLen] = '\0';

        if (szHeader[0] != '<' ||
            strstr(szHeader, "interlis.ch/INTERLIS2") == nullptr)
        {
            // "www.interlis.ch/INTERLIS2.3"
            VSIFCloseL(fp);
            return FALSE;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      We assume now that it is ILI2.  Close and instantiate a         */
    /*      ILI2Reader on it.                                               */
    /* -------------------------------------------------------------------- */
    VSIFCloseL(fp);

    poReader = CreateILI2Reader();
    if (poReader == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "File %s appears to be ILI2 but the ILI2 reader cannot\n"
                 "be instantiated, likely because Xerces support was not\n"
                 "configured in.",
                 pszNewName);
        return FALSE;
    }

    if (!osModelFilename.empty())
        poReader->ReadModel(this, poImdReader, osModelFilename);

    poReader->SetSourceFile(pszName);

    poReader->SaveClasses(pszName);

    listLayer = poReader->GetLayers();
    list<OGRLayer *>::const_iterator layerIt;
    for (layerIt = listLayer.begin(); layerIt != listLayer.end(); ++layerIt)
    {
        (*layerIt)->ResetReading();
    }

    return TRUE;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

int OGRILI2DataSource::Create(const char *pszFilename,
                              CPL_UNUSED char **papszOptions)

{
    char **filenames = CSLTokenizeString2(pszFilename, ",", 0);
    pszName = CPLStrdup(filenames[0]);
    const char *pszModelFilename =
        (CSLCount(filenames) > 1) ? filenames[1] : nullptr;

    if (pszModelFilename == nullptr)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "ILI2 Create(): model file not specified in destination filename.");
        CSLDestroy(filenames);
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Create the output file.                                         */
    /* -------------------------------------------------------------------- */

    if (strcmp(pszName, "/vsistdout/") == 0 ||
        STARTS_WITH(pszName, "/vsigzip/"))
    {
        fpOutput = VSIFOpenL(pszName, "wb");
    }
    else if (STARTS_WITH(pszName, "/vsizip/"))
    {
        if (EQUAL(CPLGetExtensionSafe(pszName).c_str(), "zip"))
        {
            char *pszNewName = CPLStrdup(
                CPLFormFilenameSafe(pszName, "out.xtf", nullptr).c_str());
            CPLFree(pszName);
            pszName = pszNewName;
        }

        fpOutput = VSIFOpenL(pszName, "wb");
    }
    else
        fpOutput = VSIFOpenL(pszName, "wb+");
    if (fpOutput == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Failed to create XTF file %s.",
                 pszName);
        CSLDestroy(filenames);
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Parse model                                                     */
    /* -------------------------------------------------------------------- */
    poImdReader->ReadModel(pszModelFilename);

    /* -------------------------------------------------------------------- */
    /*      Write headers                                                   */
    /* -------------------------------------------------------------------- */
    VSIFPrintfL(fpOutput, "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n");
    VSIFPrintfL(fpOutput,
                "<TRANSFER xmlns=\"http://www.interlis.ch/INTERLIS2.3\">\n");
    VSIFPrintfL(fpOutput,
                "<HEADERSECTION SENDER=\"OGR/GDAL %s\" VERSION=\"2.3\">\n",
                GDALVersionInfo("RELEASE_NAME"));
    VSIFPrintfL(fpOutput, "<MODELS>\n");
    for (IliModelInfos::const_iterator it = poImdReader->modelInfos.begin();
         it != poImdReader->modelInfos.end(); ++it)
    {
        VSIFPrintfL(fpOutput,
                    "<MODEL NAME=\"%s\" URI=\"%s\" VERSION=\"%s\"/>\n",
                    it->name.c_str(), it->uri.c_str(), it->version.c_str());
    }
    VSIFPrintfL(fpOutput, "</MODELS>\n");
    VSIFPrintfL(fpOutput, "</HEADERSECTION>\n");
    VSIFPrintfL(fpOutput, "<DATASECTION>\n");
    const char *basketName = poImdReader->mainBasketName.c_str();
    VSIFPrintfL(fpOutput, "<%s BID=\"%s\">\n", basketName, basketName);

    CSLDestroy(filenames);
    return TRUE;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRILI2DataSource::ICreateLayer(const char *pszLayerName,
                                const OGRGeomFieldDefn *poGeomFieldDefn,
                                CSLConstList /*papszOptions*/)
{
    if (fpOutput == nullptr)
        return nullptr;

    const auto eType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone;

    FeatureDefnInfo featureDefnInfo =
        poImdReader->GetFeatureDefnInfo(pszLayerName);
    OGRFeatureDefn *poFeatureDefn = featureDefnInfo.GetTableDefnRef();
    if (poFeatureDefn == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Layer '%s' not found in model definition. "
                 "Creating adhoc layer",
                 pszLayerName);
        poFeatureDefn = new OGRFeatureDefn(pszLayerName);
        poFeatureDefn->SetGeomType(eType);
    }
    OGRILI2Layer *poLayer =
        new OGRILI2Layer(poFeatureDefn, featureDefnInfo.poGeomFieldInfos, this);

    nLayers++;
    papoLayers = static_cast<OGRILI2Layer **>(
        CPLRealloc(papoLayers, sizeof(OGRILI2Layer *) * nLayers));
    papoLayers[nLayers - 1] = poLayer;

    return poLayer;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRILI2DataSource::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer))
        return TRUE;
    else if (EQUAL(pszCap, ODsCCurveGeometries))
        return TRUE;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRILI2DataSource::GetLayer(int iLayer)

{
    list<OGRLayer *>::const_iterator layerIt = listLayer.begin();
    int i = 0;
    while (i < iLayer && layerIt != listLayer.end())
    {
        ++i;
        ++layerIt;
    }

    if (i == iLayer && layerIt != listLayer.end())
    {
        OGRILI2Layer *tmpLayer = reinterpret_cast<OGRILI2Layer *>(*layerIt);
        return tmpLayer;
    }

    return nullptr;
}
