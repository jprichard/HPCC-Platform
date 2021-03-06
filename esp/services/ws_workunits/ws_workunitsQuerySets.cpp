/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

#include "ws_workunitsService.hpp"
#include "ws_fs.hpp"
#include "jlib.hpp"
#include "daclient.hpp"
#include "dalienv.hpp"
#include "dadfs.hpp"
#include "dfuwu.hpp"
#include "eclhelper.hpp"

const unsigned roxieQueryRoxieTimeOut = 60000;

#define SDS_LOCK_TIMEOUT (5*60*1000) // 5mins, 30s a bit short

bool isRoxieProcess(const char *process)
{
    if (!process)
        return false;
    Owned<IRemoteConnection> conn = querySDS().connect("Environment", myProcessSession(), RTM_LOCK_READ, SDS_LOCK_TIMEOUT);
    if (!conn)
        return false;
    VStringBuffer xpath("Software/RoxieCluster[@name=\"%s\"]", process);
    return conn->queryRoot()->hasProp(xpath.str());
}

void doWuFileCopy(IClientFileSpray &fs, IEspWULogicalFileCopyInfo &info, const char *logicalname, const char *cluster, bool isRoxie, bool supercopy)
{
    try
    {
        Owned<IClientCopy> req = fs.createCopyRequest();
        req->setSourceLogicalName(logicalname);
        req->setDestLogicalName(logicalname);
        req->setDestGroup(cluster);
        req->setSuperCopy(supercopy);
        if (isRoxie)
            req->setDestGroupRoxie("Yes");

        Owned<IClientCopyResponse> resp = fs.Copy(req);
        info.setDfuCopyWuid(resp->getResult());
    }
    catch (IException *e)
    {
        StringBuffer msg;
        info.setDfuCopyError(e->errorMessage(msg).str());
    }
}

bool copyWULogicalFiles(IEspContext &context, IConstWorkUnit &cw, const char *cluster, bool copyLocal, IEspWUCopyLogicalClusterFileSections &lfinfo)
{
    if (isEmpty(cluster))
        throw MakeStringException(ECLWATCH_INVALID_CLUSTER_NAME, "copyWULogicalFiles Cluster parameter not set.");

    Owned<IUserDescriptor> udesc = createUserDescriptor();
    udesc->set(context.queryUserId(), context.queryPassword());

    IArrayOf<IEspWULogicalFileCopyInfo> foreign;
    IArrayOf<IEspWULogicalFileCopyInfo> onCluster;
    IArrayOf<IEspWULogicalFileCopyInfo> notOnCluster;
    IArrayOf<IEspWULogicalFileCopyInfo> notFound;

    Owned<IClientFileSpray> fs;
    if (copyLocal)
    {
        fs.setown(createFileSprayClient());
        VStringBuffer url("http://.:%d/FileSpray", 8010);
        fs->addServiceUrl(url.str());
    }

    bool isRoxie = isRoxieProcess(cluster);

    Owned<IConstWUGraphIterator> graphs = &cw.getGraphs(GraphTypeActivities);
    ForEach(*graphs)
    {
        Owned <IPropertyTree> xgmml = graphs->query().getXGMMLTree(false);
        Owned<IPropertyTreeIterator> iter = xgmml->getElements(".//node");
        ForEach(*iter)
        {
            try
            {
                IPropertyTree &node = iter->query();
                ThorActivityKind kind = (ThorActivityKind) node.getPropInt("att[@name='_kind']/@value", TAKnone);

                if(kind==TAKdiskwrite || kind==TAKindexwrite || kind==TAKcsvwrite || kind==TAKxmlwrite)
                    continue;
                if (node.getPropBool("att[@name='_isSpill']/@value") || node.getPropBool("att[@name='_isTransformSpill']/@value"))
                    continue;

                Owned<IEspWULogicalFileCopyInfo> info = createWULogicalFileCopyInfo();
                const char *logicalname = node.queryProp("att[@name='_indexFileName']/@value");
                if (logicalname)
                    info->setIsIndex(true);
                else
                    logicalname = node.queryProp("att[@name='_fileName']/@value");
                info->setLogicalName(logicalname);
                if (logicalname)
                {
                    if (!strnicmp("~foreign::", logicalname, 10))
                        foreign.append(*info.getClear());
                    else
                    {
                        Owned<IDistributedFile> df = queryDistributedFileDirectory().lookup(logicalname, udesc);
                        if(!df)
                            notFound.append(*info.getClear());
                        else if (df->findCluster(cluster)!=NotFound)
                        {
                            onCluster.append(*info.getClear());
                        }
                        else
                        {
                            StringArray clusters;
                            df->getClusterNames(clusters);
                            info->setClusters(clusters);
                            if (copyLocal)
                            {
                                StringBuffer wuid;
                                bool supercopy = queryDistributedFileDirectory().isSuperFile(logicalname, NULL, udesc);
                                doWuFileCopy(*fs, *info, logicalname, cluster, isRoxie, supercopy);
                            }
                            notOnCluster.append(*info.getClear());
                        }
                    }
                }
            }
            catch(IException *e)
            {
                e->Release();
            }
        }
        lfinfo.setClusterName(cluster);
        lfinfo.setNotOnCluster(notOnCluster);
        lfinfo.setOnCluster(onCluster);
        lfinfo.setForeign(foreign);
        lfinfo.setNotFound(notFound);
    }

    return true;
}

void copyWULogicalFilesToTarget(IEspContext &context, IConstWUClusterInfo &clusterInfo, IConstWorkUnit &cw, IArrayOf<IConstWUCopyLogicalClusterFileSections> &clusterfiles, bool doLocalCopy)
{
    const StringArray &thors = clusterInfo.getThorProcesses();
    ForEachItemIn(i, thors)
    {
        Owned<IEspWUCopyLogicalClusterFileSections> files = createWUCopyLogicalClusterFileSections();
        copyWULogicalFiles(context, cw, thors.item(i), doLocalCopy, *files);
        clusterfiles.append(*files.getClear());
    }
    SCMStringBuffer roxie;
    clusterInfo.getRoxieProcess(roxie);
    if (roxie.length())
    {
        Owned<IEspWUCopyLogicalClusterFileSections> files = createWUCopyLogicalClusterFileSections();
        copyWULogicalFiles(context, cw, roxie.str(), doLocalCopy, *files);
        clusterfiles.append(*files.getClear());
    }
}

bool CWsWorkunitsEx::onWUCopyLogicalFiles(IEspContext &context, IEspWUCopyLogicalFilesRequest &req, IEspWUCopyLogicalFilesResponse &resp)
{
    if (isEmpty(req.getWuid()))
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT, "WUCopyLogicalFiles WUID parameter not set.");

    Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
    Owned<IConstWorkUnit> cw = factory->openWorkUnit(req.getWuid(), false);
    if (!cw)
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT,"Cannot open workunit %s", req.getWuid());

    resp.setWuid(req.getWuid());

    SCMStringBuffer cluster;
    if (notEmpty(req.getCluster()))
        cluster.set(req.getCluster());
    else
        cw->getClusterName(cluster);

    Owned <IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(cluster.str());

    IArrayOf<IConstWUCopyLogicalClusterFileSections> clusterfiles;
    copyWULogicalFilesToTarget(context, *clusterInfo, *cw, clusterfiles, req.getCopyLocal());
    resp.setClusterFiles(clusterfiles);

    return true;
}

bool CWsWorkunitsEx::onWUPublishWorkunit(IEspContext &context, IEspWUPublishWorkunitRequest & req, IEspWUPublishWorkunitResponse & resp)
{
    if (isEmpty(req.getWuid()))
        throw MakeStringException(ECLWATCH_NO_WUID_SPECIFIED,"No Workunit ID has been specified.");

    Owned<IWorkUnitFactory> factory = getWorkUnitFactory(context.querySecManager(), context.queryUser());
    Owned<IConstWorkUnit> cw = factory->openWorkUnit(req.getWuid(), false);
    if (!cw)
        throw MakeStringException(ECLWATCH_CANNOT_OPEN_WORKUNIT,"Cannot find the workunit %s", req.getWuid());

    resp.setWuid(req.getWuid());

    SCMStringBuffer queryName;
    if (notEmpty(req.getJobName()))
        queryName.set(req.getJobName());
    else
        cw->getJobName(queryName).str();

    SCMStringBuffer cluster;
    if (notEmpty(req.getCluster()))
        cluster.set(req.getCluster());
    else
        cw->getClusterName(cluster);

    Owned <IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(cluster.str());

    SCMStringBuffer queryset;
    clusterInfo->getQuerySetName(queryset);

    WorkunitUpdate wu(&cw->lock());
    if (notEmpty(req.getJobName()))
        wu->setJobName(req.getJobName());

    StringBuffer queryId;
    addQueryToQuerySet(wu, queryset.str(), queryName.str(), NULL, (WUQueryActivationOptions)req.getActivate(), queryId);
    wu->commit();
    wu.clear();

    if (queryId.length())
        resp.setQueryId(queryId.str());
    resp.setQueryName(queryName.str());
    resp.setQuerySet(queryset.str());

    if (req.getCopyLocal() || req.getShowFiles())
    {
        IArrayOf<IConstWUCopyLogicalClusterFileSections> clusterfiles;
        copyWULogicalFilesToTarget(context, *clusterInfo, *cw, clusterfiles, req.getCopyLocal());
        resp.setClusterFiles(clusterfiles);
    }

    return true;
}

bool CWsWorkunitsEx::onWUQuerysets(IEspContext &context, IEspWUQuerysetsRequest & req, IEspWUQuerysetsResponse & resp)
{
    Owned<IPropertyTree> queryRegistry = getQueryRegistryRoot();
    if (!queryRegistry)
        return false;

    IArrayOf<IEspQuerySet> querySets;
    Owned<IPropertyTreeIterator> it = queryRegistry->getElements("QuerySet");
    ForEach(*it)
    {
        Owned<IEspQuerySet> qs = createQuerySet("", "");
        qs->setQuerySetName(it->query().queryProp("@id"));
        querySets.append(*qs.getClear());

    }
    resp.setQuerysets(querySets);
    return true;
}

bool CWsWorkunitsEx::onWUQuerysetDetails(IEspContext &context, IEspWUQuerySetDetailsRequest & req, IEspWUQuerySetDetailsResponse & resp)
{
    resp.setQuerySetName(req.getQuerySetName());

    Owned<IPropertyTree> registry = getQueryRegistry(req.getQuerySetName(), true);
    if (!registry)
        return false;

    IArrayOf<IEspQuerySetQuery> querySetQueries;
    Owned<IPropertyTreeIterator> queries = registry->getElements("Query");
    ForEach(*queries)
    {
        IPropertyTree &query = queries->query();
        Owned<IEspQuerySetQuery> q = createQuerySetQuery("", "");
        q->setId(query.queryProp("@id"));
        q->setName(query.queryProp("@name"));
        q->setDll(query.queryProp("@dll"));
        q->setWuid(query.queryProp("@wuid"));
        q->setSuspended(query.getPropBool("@suspended", false));
        querySetQueries.append(*q.getLink());

    }
    resp.setQuerysetQueries(querySetQueries);

    IArrayOf<IEspQuerySetAlias> querySetAliases;
    Owned<IPropertyTreeIterator> aliases = registry->getElements("Alias");
    ForEach(*aliases)
    {
        IPropertyTree &alias = aliases->query();
        Owned<IEspQuerySetAlias> a = createQuerySetAlias("", "");
        a->setName(alias.queryProp("@name"));
        a->setId(alias.queryProp("@id"));
        querySetAliases.append(*a.getClear());
    }
    resp.setQuerysetAliases(querySetAliases);
    return true;
}

bool CWsWorkunitsEx::onWUQuerysetActionQueries(IEspContext &context, IEspWUQuerySetActionQueriesRequest & req, IEspWUQuerySetActionQueriesResponse & resp)
{
    resp.setQuerySetName(req.getQuerySetName());
    resp.setRemove(req.getRemove());

    Owned<IPropertyTree> queryRegistry = getQueryRegistry(req.getQuerySetName(), false);

    IArrayOf<IEspQuerySetQueryAction> actions;
    ForEachItemIn(qa, req.getQuerysetQueryActions())
    {
        IConstQuerySetQueryAction& item=req.getQuerysetQueryActions().item(qa);
        if(notEmpty(item.getId()))
        {
            if (req.getRemove())
            {
                removeAliasesFromNamedQuery(queryRegistry, item.getId());
                removeNamedQuery(queryRegistry, item.getId());

                Owned<IEspQuerySetQueryAction> action = createQuerySetQueryAction("", "");
                action->setId(item.getId());
                action->setStatus("Completed");
                actions.append(*action.getClear());
            }
            if (req.getToggleSuspend())
            {
                setQuerySuspendedState(queryRegistry, item.getId(), !item.getSuspended());

                Owned<IEspQuerySetQueryAction> action = createQuerySetQueryAction("", "");
                action->setId(item.getId());
                action->setStatus("Completed");
                actions.append(*action.getClear());
            }
        }
    }
    resp.setQuerysetQueryActions(actions);
    return true;
}

bool CWsWorkunitsEx::onWUQuerysetActionAliases(IEspContext &context, IEspWUQuerySetActionAliasesRequest & req, IEspWUQuerySetActionAliasesResponse & resp)
{
    resp.setQuerySetName(req.getQuerySetName());
    resp.setRemove(req.getRemove());

    Owned<IPropertyTree> queryRegistry = getQueryRegistry(req.getQuerySetName(), false);

    IArrayOf<IEspQuerySetAliasAction> actions;
    ForEachItemIn(aa, req.getQuerysetAliasActions())
    {
        IConstQuerySetAliasAction& item=req.getQuerysetAliasActions().item(aa);
        if (req.getRemove())
        {
            if(notEmpty(item.getId()))
            {
                removeAliasesFromNamedQuery(queryRegistry, item.getId());
                Owned<IEspQuerySetAliasAction> action = createQuerySetAliasAction("", "");
                action->setId(item.getId());
                action->setStatus("Completed");
                actions.append(*action.getClear());

            }
        }
    }
    resp.setQuerysetAliasActions(actions);
    return true;
}
